#!/usr/bin/env python3
"""Compile production camera identity helpers against bounded host I/O scenarios."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/dev/annibale/annibale_camera_hw.inc"


def function(text, name):
    matches = list(re.finditer(r"static (?:bool|void|uint16_t)\s+" + name + r"\(", text))
    if len(matches) != 1:
        raise AssertionError("expected one production function: " + name)
    start = matches[0].start()
    at = text.index("{", matches[0].end()) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


# Expected clear addresses in reg_wr are literals from native cam_cci_irq.
# Source macros select actual accesses; changing them must fail this fixture.
PRELUDE = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NSEC_PER_USEC 1000
#define NSEC_PER_MSEC 1000000
#define say(...) ((void)0)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static bool cci_ready;
static uintptr_t cci0_base=1, tlmm_base=1;
static uint16_t wide_chip_id, front_chip_id;
static uint8_t wide_slave_addr, front_slave_addr, wide_bus_mode;
static unsigned started, writes, data_reads, stale_reads, status_reads, delays;
static unsigned clear_pending, clear_pairs;
static unsigned stale_count, stale_left, fifo_level, status_at;
static uint32_t statuses[4], raw_word;
static bool init_ok;
static int gpio_fail;
static unsigned gpio_clock, gpio_bit, stops, rear_gpio, front_gpio;
static uint16_t rear_word=0x5650, front_word=0x2042;
static bool sda_high[128];
static unsigned tx_count, start_count;
static int tx_fail, start_fail;
static uint8_t tx_trace[8];
static uint32_t queue_words[4];
static unsigned queue_count;
static void delay_for_interval(unsigned n, unsigned unit) { (void)n; (void)unit; delays++; }
static uint32_t reg_rd(uintptr_t base, uint32_t off) {
    (void)base;
    CHECK(!clear_pending);
    if (off == 0x000) return init_ok ? 0x10000000u : 0;
    if (off == CCI_I2C_M1_READ_BUF_LEVEL) {
        return started ? fifo_level : (stale_reads < stale_count ? stale_count-stale_reads : stale_left);
    }
    if (off == CCI_IRQ_STATUS_0) {
        status_reads++;
        unsigned i=status_at++;
        return statuses[i < 4 ? i : 3];
    }
    if (off == CCI_I2C_M1_READ_DATA) {
        if (started) {
            data_reads++;
            CHECK(status_reads && (statuses[(status_at-1)<4 ? status_at-1 : 3] & 0x11000u)==0x11000u);
        } else stale_reads++;
        return raw_word;
    }
    return 0;
}
static void reg_wr(uintptr_t base, uint32_t off, uint32_t value) {
    (void)base; writes++;
    if (clear_pending) {
        CHECK(off == 0xc00 && value == 1);
        clear_pending=0; clear_pairs++; return;
    }
    CHECK(off != 0xc00 && off != 0xc10);
    if (off == 0xc08) { CHECK(value == 0xffffffffu); clear_pending=1; }
    if (off == CCI_I2C_M1_Q0_LOAD_DATA) { CHECK(queue_count < 4); queue_words[queue_count++]=value; }
    if (off == CCI_QUEUE_START) {
        CHECK(queue_count==3 || queue_count==4);
        if (queue_count==4) {
            CHECK(queue_words[0]==0x10101 && queue_words[1]==0x0a3029 && queue_words[2]==0x2a && queue_words[3]==0x108);
        }
        queue_count=0; started++; status_at=0;
    }
}
static void od_i2c_sda(uint32_t pin, bool high) {
    CHECK(pin < 128); sda_high[pin]=high;
    if (pin==115) rear_gpio++;
    if (pin==4) front_gpio++;
}
static bool od_i2c_scl_high(uint32_t pin) {
    unsigned at=gpio_clock++;
    if (gpio_fail < 0 && start_count==2) {
        if(at==8) CHECK(!sda_high[pin==5?4:115]);
        if(at==17) CHECK(sda_high[pin==5?4:115]);
    }
    return (int)at != gpio_fail;
}
static void od_i2c_scl_low(uint32_t pin) { (void)pin; }
static bool tlmm_get_in(uint32_t pin) { return ((pin==4?front_word:rear_word) >> (15-(gpio_bit++ % 16))) & 1; }
static bool od_i2c_start(uint32_t sda, uint32_t scl) { (void)sda; (void)scl; return (int)start_count++ != start_fail; }
static bool od_i2c_tx_byte(uint32_t sda, uint32_t scl, uint8_t b) { (void)sda; (void)scl; tx_trace[tx_count % 8]=b; if(tx_count % 4==3) gpio_clock=gpio_bit=0; return (int)tx_count++ != tx_fail; }
static void od_i2c_stop(uint32_t sda, uint32_t scl) { (void)sda; (void)scl; stops++; }
static void reset(void) {
    cci_ready=true; init_ok=true; clear_pending=clear_pairs=0;
    started=writes=data_reads=stale_reads=status_reads=delays=0;
    stale_count=stale_left=status_at=0; fifo_level=1;
    raw_word=0x505621; for(unsigned i=0;i<4;i++) statuses[i]=0x11000;
    gpio_fail=tx_fail=start_fail=-1; gpio_clock=gpio_bit=stops=rear_gpio=front_gpio=0;
    rear_word=0x5650; front_word=0x2042; tx_count=start_count=queue_count=0;
    memset(tx_trace,0,sizeof(tx_trace)); memset(sda_high,0,sizeof(sda_high));
}
"""

MAIN = r"""
static void failed_read(void) {
    uint16_t value=0xcc;
    CHECK(!cci0_m1_read16(0x10,0x300a,&value));
    CHECK(!cci_ready && value==0xcc && data_reads==0);
    unsigned before=writes;
    CHECK(!cci0_m1_read16(0x10,0x300b,&value));
    CHECK(writes==before);
}
int main(void) {
    unsigned count=0;
    uint16_t value;
    reset(); value=0;
    CHECK(cci0_m1_read16(0x10,0x300a,&value));
    CHECK(value==0x5650 && data_reads==1 && cci_ready); count++;
    reset(); statuses[0]=0; statuses[1]=0x1000;
    CHECK(cci0_m1_read16(0x10,0x300a,&value));
    CHECK(status_reads==3 && data_reads==1); count++;
    reset(); statuses[0]=0x10000; statuses[1]=0x10000;
    CHECK(cci0_m1_read16(0x10,0x300a,&value));
    CHECK(status_reads==3 && data_reads==1); count++;
    for(unsigned which=0;which<3;which++) {
        reset(); for(unsigned i=0;i<4;i++) statuses[i]=which==0?0:(which==1?0x1000:0x10000);
        failed_read(); CHECK(status_reads==250); count++;
    }
    for(unsigned bit=0;bit<32;bit++) {
        if (!(0x60ee6000u & (1u<<bit))) continue;
        reset(); statuses[0] |= 1u<<bit;
        failed_read(); CHECK(status_reads==1); count++;
    }
    reset(); fifo_level=0; failed_read(); count++;
    reset(); fifo_level=2; failed_read(); count++;
    reset(); stale_count=2;
    CHECK(cci0_m1_read16(0x10,0x300a,&value)); CHECK(stale_reads==2 && data_reads==1); count++;
    reset(); stale_count=64;
    CHECK(cci0_m1_read16(0x10,0x300a,&value)); CHECK(stale_reads==64 && data_reads==1); count++;
    reset(); stale_count=65; failed_read(); CHECK(started==0 && stale_reads==0); count++;
    reset(); stale_count=1; stale_left=1; failed_read(); CHECK(started==0); count++;
    reset(); value=0;
    CHECK(od_i2c_read16_reg16(4,5,0x10,0x300a,&value));
    CHECK(value==0x2042 && stops==1 && gpio_clock==18); count++;
    CHECK(tx_count==4 && start_count==2 && tx_trace[0]==0x20 && tx_trace[1]==0x30 && tx_trace[2]==0x0a && tx_trace[3]==0x21);
    for(int clock=0;clock<18;clock++) {
        reset(); gpio_fail=clock; value=0xcc;
        CHECK(!od_i2c_read16_reg16(4,5,0x10,0x300a,&value));
        CHECK(value==0xcc && stops==1); count++;
    }
    reset(); statuses[0]=0x20000000; probe_sensors_all(true);
    CHECK(started==1 && !cci_ready && wide_chip_id==0 && wide_bus_mode==0 && rear_gpio==0);
    CHECK(front_chip_id==0x2042 && front_gpio>0); count++;
    reset(); init_ok=false; probe_sensors_all(true);
    CHECK(started==0 && rear_gpio>0 && wide_chip_id==0x5650 && wide_bus_mode==2); count++;
    CHECK(count == 43);
    for(int point=0;point<4;point++) {
        reset(); tx_fail=point; value=0xbeef;
        CHECK(!od_i2c_read16_reg16(4,5,0x10,0x300b,&value)); CHECK(value==0xbeef && stops==1); count++;
    }
    for(int point=0;point<2;point++) {
        reset(); start_fail=point; value=0xbeef;
        CHECK(!od_i2c_read16_reg16(4,5,0x10,0x300b,&value)); CHECK(value==0xbeef && stops==(point?1u:0u)); count++;
    }
    reset(); probe_sensors_all(true);
    CHECK(started==1 && wide_chip_id==0x5650 && wide_bus_mode==1 && front_chip_id==0x2042);
    CHECK(rear_gpio==0 && front_gpio>0 && tx_count==4 && tx_trace[2]==0x0b); count++;
    reset(); raw_word=0x555621; probe_sensors_all(true);
    CHECK(cci_ready && started==1 && rear_gpio>0 && wide_chip_id==0x5650 && wide_bus_mode==2); count++;
    reset(); raw_word=0x555621; rear_word=0x5655; front_word=0x5620; probe_sensors_all(true);
    CHECK(wide_chip_id==0 && front_chip_id==0 && wide_bus_mode==0 && started==1 && tx_count==8); count++;
    reset(); tlmm_base=0; probe_sensors_all(true);
    CHECK(wide_chip_id==0x5650 && front_chip_id==0 && rear_gpio==0 && front_gpio==0); tlmm_base=1; count++;
    reset(); probe_sensors_all(false); CHECK(started==0 && wide_bus_mode==2 && front_chip_id==0x2042); count++;
    CHECK(sensor_id_matches(0x5650,0x5650,0xffff)); count++;
    CHECK(sensor_id_matches(0x2042,0x2042,0xffffffff)); count++;
    CHECK(!sensor_id_matches(0x5620,0x2042,0xffffffff)); count++;
    CHECK(sensor_id_matches(0x365f,0x65,0x0ff0)); count++;
    CHECK(sensor_id_matches(0x5650,0x5650,0)); count++;
    CHECK(!sensor_id_matches(0,0x5650,0xffff) && !sensor_id_matches(0xffff,0x5650,0xffff)); count++;
    reset(); CHECK(cci0_m1_init()); CHECK(clear_pairs == 2);
    CHECK(cci0_m1_read16(0x10,0x300a,&value)); CHECK(clear_pairs == 3);
    CHECK(cci0_m1_write_bytes(0x10,0x30,0x0a,0,2));
    CHECK(clear_pairs == 4 && !clear_pending);
    printf("%u identity scenarios and MMIO address/order checks passed\n",count);
    return 0;
}
"""


def compile_and_run(directory, label, source):
    constants = "\n".join(line for line in source.splitlines()
                          if line.startswith(("#define CCI_", "#define GPIO_", "#define SENSOR_", "#define WIDE_SENSOR_", "#define FRONT_SENSOR_")))
    names = ("cci0_m1_init", "cci0_m1_read16", "cci0_m1_write_bytes", "od_i2c_rx_byte",
             "od_i2c_read16_reg16", "sensor_id_matches", "probe_sensors_all")
    body = "\n".join(function(source, name) for name in names)
    path = directory / (label + ".c")
    binary = directory / label
    path.write_text(constants + "\n" + PRELUDE + body + MAIN)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(binary)], check=True, capture_output=True, text=True, timeout=30)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)


def main():
    source = SOURCE.read_text()
    rx = function(source, "od_i2c_rx_byte")
    mutations = {
        "sid-prefix": ("(raw & 0xff00u) | ((raw >> 16) & 0xffu)", "raw & 0xffffu"),
        "read-width": ("0x0au | (2u << 4)", "0x0au | (1u << 4)"),
        "cci-byte-order": ("(raw & 0xff00u) | ((raw >> 16) & 0xffu)", "(raw >> 8) & 0xffffu"),
        "gpio-byte-order": ("((uint16_t)hi << 8) | lo", "((uint16_t)lo << 8) | hi"),
        "first-byte-ack": ("scl_pin, true, &hi)", "scl_pin, false, &hi)"),
        "last-byte-nack": ("scl_pin, false, &lo)", "scl_pin, true, &lo)"),
        "partial-output": ("if (ok) {\n\t\t*out_val", "if (true) {\n\t\t*out_val"),
        "front-register": ("#define FRONT_SENSOR_ID_REG           0x300bu", "#define FRONT_SENSOR_ID_REG           0x300au"),
        "expected-id": ("return raw == expected;", "(void)raw; (void)expected; return true;"),
        "mask-shift": ("while ((id_mask & 1u) == 0)", "while (false)"),
        "mask-default": ("id_mask = 0xffffu;", "id_mask = 1u;"),
        "completion": ("(irq & (CCI_IRQ_M1_RD_DONE | CCI_IRQ_M1_Q0_REPORT)) ==\n\t\t    "
                       "(CCI_IRQ_M1_RD_DONE | CCI_IRQ_M1_Q0_REPORT)",
                       "reg_rd(cci0_base, CCI_I2C_M1_READ_BUF_LEVEL) > 0"),
        "error-irq": ("if (irq & CCI_IRQ_M1_ERROR)", "if (false)"),
        "fifo-size": ("if (level == 1u)", "if (level > 0u)"),
        "data-clock": (rx, rx.replace("if (!od_i2c_scl_high(scl_pin)) {\n\t\t\tod_i2c_scl_low(scl_pin);",
                                     "if ((od_i2c_scl_high(scl_pin), false)) {\n\t\t\tod_i2c_scl_low(scl_pin);", 1)),
        "ack-clock": (rx, rx.replace("bool ack_clock_ok = od_i2c_scl_high(scl_pin);",
                                    "bool ack_clock_ok = (od_i2c_scl_high(scl_pin), true);", 1)),
        "gpio-fallback": (" && (!cci_initialized || cci_ready)", ""),
        "global-address": ("#define CCI_IRQ_GLOBAL_CLEAR_CMD        0xc00u",
                           "#define CCI_IRQ_GLOBAL_CLEAR_CMD        0xc10u"),
        "global-write": ("reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);", "(void)0;"),
        "clear-order": ("reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);\n\t"
                        "reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);",
                        "reg_wr(cci0_base, CCI_IRQ_GLOBAL_CLEAR_CMD, 1u);\n\t"
                        "reg_wr(cci0_base, CCI_IRQ_CLEAR_0, 0xffffffffu);"),
    }
    with tempfile.TemporaryDirectory(prefix="camera-identity-") as temp:
        directory = Path(temp)
        result = compile_and_run(directory, "production", source)
        if result.returncode:
            raise AssertionError(result.stderr)
        print("PASS: " + result.stdout.strip())
        for name, (old, new) in mutations.items():
            changed = source.replace(old, new)
            if changed == source:
                raise AssertionError("mutation did not change source: " + name)
            result = compile_and_run(directory, name, changed)
            if result.returncode == 0:
                raise AssertionError("mutation was not detected: " + name)
        print("PASS: " + str(len(mutations)) + " identity/transport/MMIO mutations detected")


if __name__ == "__main__":
    main()
