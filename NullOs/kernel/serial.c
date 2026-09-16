#include "../include/serial.h"
#include "../include/string.h"
#include "../include/vga.h"

static u16 serial_port = COM1;

static void serial_wait_tx(void) {
    // Wait until Transmitter Holding Register is empty
    while ((inb(SERIAL_LINE_STS(serial_port)) & SERIAL_LSR_THRE) == 0);
}

void serial_init(void) {
    serial_port = COM1;

    // Disable all interrupts
    outb(SERIAL_INT_EN(serial_port), 0x00);

    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_LINE_CTRL(serial_port), 0x80);

    // Set divisor to 1 (115200 baud) — lo byte, then hi byte
    outb(SERIAL_DATA_REG(serial_port), 0x01);
    outb(SERIAL_INT_EN(serial_port), 0x00);

    // 8 bits, no parity, one stop bit (8N1)
    outb(SERIAL_LINE_CTRL(serial_port), 0x03);

    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_FIFO_CTRL(serial_port), 0xC7);

    // IRQs enabled, RTS/DSR set
    outb(SERIAL_MODEM_CTRL(serial_port), 0x0B);

    // Enable interrupts for received data (optional, for IRQ-driven RX)
    // We use polling for simplicity
    outb(SERIAL_INT_EN(serial_port), 0x00);

    vga_print("[SERIAL] COM1 initialized at 115200 baud (8N1)\n");
}

void serial_putchar(char c) {
    serial_wait_tx();
    outb(SERIAL_DATA_REG(serial_port), (u8)c);
}

bool serial_tx_ready(void) {
    return (inb(SERIAL_LINE_STS(serial_port)) & SERIAL_LSR_THRE) != 0;
}

bool serial_rx_ready(void) {
    return (inb(SERIAL_LINE_STS(serial_port)) & SERIAL_LSR_DR) != 0;
}

int serial_getchar(void) {
    if (!serial_rx_ready()) return -1;
    return inb(SERIAL_DATA_REG(serial_port));
}

void serial_print(const char* str) {
    while (*str) {
        serial_putchar(*str++);
    }
}

// Context for serial printf
static void serial_out_char(char c, void* ctx) {
    (void)ctx;
    serial_putchar(c);
}

int serial_printf(const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = kprintf_format(serial_out_char, NULL, fmt, args);
    __builtin_va_end(args);
    return ret;
}
