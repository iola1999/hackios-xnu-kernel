/*
 * What the Bluetooth files share. annibale_bt_uart.c drives SE14, the 4-wire UART to the WCN7850's Bluetooth
 * half; annibale_bt_hci.c brings the chip up over it (boot-arg bt=N). The UART side moves H4 bytes and knows
 * nothing of HCI, so a later consumer (bluetoothd's channel) can take the chip over through the same calls.
 */
#ifndef ANNIBALE_BT_PRIVATE_H
#define ANNIBALE_BT_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include "mhi_pcie_private.h"   /* mhi_pcie_say: the line goes to the loader's log as well */

#define bt_say mhi_pcie_say

#define BT_BAUD                 115200u
#define BT_BAUD_HIGH            3000000u

/* clocks, pins and the engine's own checks (protocol, FIFO interface); false after saying why */
bool annibale_bt_uart_open(void);
/* 8N1, RTS/CTS flow control, BT_BAUD at that oversampling (16 or 32); the receiver started, the buffer emptied */
bool annibale_bt_uart_config(uint32_t oversampling);
/* dynamic baud rate switch (115200 <-> 3000000): reconfigures S6 RCG clock & GENI dividers */
bool annibale_bt_uart_set_baud(uint32_t baud);
uint32_t annibale_bt_uart_get_baud(void);
/* waits until the engine has sent every byte (the chip's CTS may hold it); the RX FIFO is drained meanwhile */
bool annibale_bt_uart_write(const uint8_t *data, uint32_t length);
/* the RX FIFO into the receive buffer, whole words while there is room; how many bytes came */
uint32_t annibale_bt_uart_poll(void);
void annibale_bt_uart_irq_clear(void);
/* the received bytes nobody has taken yet, oldest first; valid until the next poll or take */
const uint8_t *annibale_bt_uart_rx(uint32_t *length);
void annibale_bt_uart_take(uint32_t length);
void annibale_bt_uart_status(const char *when);
void annibale_bt_hex(const char *what, const uint8_t *data, uint32_t length);

/* WCN7850 vendor command 0xFC48 baud rate transition sequence */
bool annibale_bt_hci_set_baudrate(uint32_t baud);

/* bt=4 (annibale_bt_ios.c): /dev/uart.bluetooth, com.apple.uart.sk.bluetooth and the Skywalk nexus for bluetoothd */
void annibale_bt_ios_early(void);
void annibale_bt_ios_serve(const uint8_t addr[6]);
void annibale_bt_uart_isr(void *ctx, uint32_t intid);

#endif

