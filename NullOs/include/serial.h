#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// COM ports
#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

// Serial port register offsets
#define SERIAL_DATA_REG(base)    ((base) + 0)  // Data register (read/write)
#define SERIAL_INT_EN(base)     ((base) + 1)  // Interrupt Enable Register
#define SERIAL_FIFO_CTRL(base)  ((base) + 2)  // FIFO Control Register
#define SERIAL_LINE_CTRL(base)  ((base) + 3)  // Line Control Register
#define SERIAL_MODEM_CTRL(base) ((base) + 4)  // Modem Control Register
#define SERIAL_LINE_STS(base)   ((base) + 5)  // Line Status Register
#define SERIAL_MODEM_STS(base)  ((base) + 6)  // Modem Status Register
#define SERIAL_SCRATCH(base)    ((base) + 7)  // Scratch Register

// Line Status Register bits
#define SERIAL_LSR_DR      0x01  // Data Ready
#define SERIAL_LSR_OE      0x02  // Overrun Error
#define SERIAL_LSR_PE      0x04  // Parity Error
#define SERIAL_LSR_FE      0x08  // Framing Error
#define SERIAL_LSR_BI      0x10  // Break Interrupt
#define SERIAL_LSR_THRE    0x20  // Transmitter Holding Register Empty
#define SERIAL_LSR_TEMT    0x40  // Transmitter Empty
#define SERIAL_LSR_FIFE    0x80  // FIFO Error

// Interrupt Enable Register bits
#define SERIAL_IER_ERBFI   0x01  // Enable Received Data Available Interrupt
#define SERIAL_IER_ETBEI   0x02  // Enable Transmitter Holding Register Empty Interrupt
#define SERIAL_IER_ELSI    0x04  // Enable Receiver Line Status Interrupt
#define SERIAL_IER_EDSSI   0x08  // Enable Modem Status Interrupt

// Modem Control Register bits
#define SERIAL_MCR_DTR     0x01  // Data Terminal Ready
#define SERIAL_MCR_RTS     0x02  // Request To Send
#define SERIAL_MCR_OUT1    0x04  // Output 1
#define SERIAL_MCR_OUT2    0x08  // Output 2 (interrupt enable)
#define SERIAL_MCR_LOOP    0x10  // Loopback mode

// Initialize COM1 serial port
void serial_init(void);

// Send a single byte
void serial_putchar(char c);

// Check if transmit buffer is empty
bool serial_tx_ready(void);

// Receive a byte (non-blocking, returns -1 if none)
int serial_getchar(void);

// Check if data is available
bool serial_rx_ready(void);

// Print a string
void serial_print(const char* str);

// Formatted print to serial
int serial_printf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_H
