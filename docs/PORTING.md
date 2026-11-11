# Porting HackiOS to another device

This guide retains the upstream workflow for starting a Qualcomm device port. The original bring-up used the POCO X5 Pro 5G (Snapdragon 778G). Hardware addresses, firmware and boot procedures need validation for each new device.

The current annibale port targets REDMI K90, SM8750, Adreno 830, WCN7850 and iOS 26.6.2. Its build and deployment steps are in the workspace [development guide](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/development.md) and [device test playbook](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/plan/device-test-playbook.md).

The profile commands below are for developing another device profile; K90 uses the workspace's build, packaging and recovery tools.

Start a new profile with:

```sh
common/new-device.sh <codename> path/to/device.dtb
```

It writes `common/devices/<codename>.conf` (the profile `common/build.sh` and `common/run.sh` read) and `common/devices/<codename>.platform.c` (the loader's description of the SoC), filling in what the device tree says. Get the device tree from the phone's `vendor_boot` image, or from a running Android: `adb -s "$ANDROID_SERIAL" pull /sys/firmware/fdt`.

## 1. The UEFI

The phone needs a UEFI that our loader can run under. Port [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to it (`uefi/annibale/` holds the files of the REDMI K90 port):

- `Platforms/<Vendor>/<codename>Pkg`: memory map and driver list (copy the closest device of the same SoC);
- `Binaries/<codename>`: the UEFI drivers of the phone's own firmware, extracted with [UEFIReader](https://github.com/WOA-Project/UEFIReader) from its `xbl.img`;
- apply our two changes from `uefi/mu-silicium.patch`: the boot shim copies the "XNUP" container after the UEFI image to `PAYLOAD_BASE`, and `BootManagerLib` starts the XNU loader from it instead of the menu.

Put the addresses in the profile: `FD_BASE`, `FD_SIZE` (where the shim puts the UEFI image) and `PAYLOAD_BASE` (free RAM for the kernel and images). `mkxnuimg.py` builds the shim from your Mu-Silicium tree with these addresses.

## 2. The loader

`xnuloader/platform_private.h` describes each device with a `struct platform` and picks it by the framebuffer base the UEFI reports. Paste `common/devices/<codename>.platform.c` next to `platform_annibale` and add it to `platforms[]`:

| Field | Where it comes from |
|---|---|
| `io_base`, `io_size` | one window that covers the SoC's peripherals; it must not start at 0 (XNU then skips clock setup) |
| `gic_dist`, `gic_redist` | the `arm,gic-v3` node's `reg` (filled in by `new-device.sh`) |
| `cores`, `core_shift` | the `cpu@` nodes; Qualcomm numbers cores in MPIDR's second affinity field (shift 8) |
| `small_cores` | a bit per core of the little cluster (lowest `capacity-dmips-mhz`) |
| `psci_hvc` | 1 if the firmware's PSCI is called with HVC (QEMU), 0 for SMC (phones) |
| `fb_base` | the framebuffer address; the loader prints it at start (`display W x H at ...`) |

Rebuild with `xnuloader/build.sh`; QEMU must still pick `qemu-virt` (no match = ramfb).

## 3. The kernel

What is generic: GICv3, the timer, PSCI SMP, the display (the UEFI's framebuffer), APFS and ramfs, the driver replacements, the return-to-loader path. What is annibale-specific and needs looking at:

- **touch**: `xnu/files/bsd/dev/annibale/annibale_goodix.c` feeds `/dev/annibale-touch`; input delivery is in `xnu/files/bsd/dev/annibale/annibale_input.c`. Another panel needs its own transport and report handling.
- **buttons, CPU frequency, watchdog**: check GPIO/PMIC definitions, frequency control and reset handling in `xnu/files/bsd/dev/annibale/annibale_input.c`, `annibale_cpufreq.c` in the same directory, and `xnu/files/osfmk/arm64/loader_return.c`.
- **GPU**: `xnu/files/bsd/dev/adreno8/` implements Adreno 830 support. The older Adreno 642L bring-up belongs to the original Snapdragon 778G target.
- **Wi-Fi**: WCN7850 uses ath12k through `wifi/lx/`, built by `tools/mk-ath12k.sh`; `xnu/files/bsd/dev/annibale/annibale_sta.c` implements the iOS station interface. The original WCN6750/ath11k path is historical.
- **audio and shared Qualcomm services**: start at `xnu/files/bsd/dev/annibale/annibale_audio.c`; confirm the new target's SMEM, SMP2P, GLINK, QRTR and QMI contracts, addresses and firmware before reuse.

Start by validating the loader, CPU and framebuffer, then input and userland startup. A lock-screen result must be established on the new target.

## 4. Firmware

Check the distribution terms for each firmware file. K90's redistributable GPU firmware and its device-specific signed firmware are documented in the userland [Adreno 830 firmware guide](https://github.com/iola1999/hackios-xnu-userland/blob/k90-port/firmware-gen8/README.md). Wi-Fi and Bluetooth extraction steps are in the workspace [development guide](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/development.md#4-提取硬件固件).

A new profile's `FIRMWARE` list must match that device; inspect the packaged files and initialization log before testing a driver.

## 5. Run

These commands exercise the generic profile workflow. Current K90 hardware runs follow the workspace device test playbook linked above, including loader recovery and per-run cleanup.

```sh
common/build.sh <codename>
common/run.sh <codename>
```

The first useful milestone is the loader's own screen and log over USB (`./xl log`), the next one is the kernel's verbose boot text (`-v`), then SpringBoard. Read [checks.md](checks.md) before trusting a "it is not in the log".
