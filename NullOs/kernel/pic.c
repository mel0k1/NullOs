#include "../include/pic.h"

// Функция ожидания для синхронизации с PIC
static inline void io_wait() {
    outb(0x80, 0);
}

void pic_init() {
    // Start initialization (ICW1)
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    // Устанавливаем новые векторы прерываний (ICW2)
    outb(PIC1_DATA, IRQ_OFFSET);
    io_wait();
    outb(PIC2_DATA, IRQ_OFFSET + 8);
    io_wait();

    // Настраиваем каскадирование (ICW3)
    outb(PIC1_DATA, 0x04);  // PIC2 подключен к IRQ2
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    // Устанавливаем режим 8086 (ICW4)
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    // Unmask: IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade to PIC2)
    outb(PIC1_DATA, 0xF8);   // Unmask IRQ0, IRQ1, IRQ2; mask IRQ3-7
    outb(PIC2_DATA, 0xFF);   // Mask all slave IRQs (RTC will unmask IRQ8 itself)
}

void pic_send_eoi(u8 irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(u8 irq) {
    u16 port;
    u8 value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_unset_mask(u8 irq) {
    u16 port;
    u8 value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

u16 pic_get_irr() {
    outb(PIC1_COMMAND, 0x0A);  // OCW3: read IRR
    outb(PIC2_COMMAND, 0x0A);
    return (inb(PIC2_DATA) << 8) | inb(PIC1_DATA);  // read from DATA port
}

u16 pic_get_isr() {
    outb(PIC1_COMMAND, 0x0B);  // OCW3: read ISR
    outb(PIC2_COMMAND, 0x0B);
    return (inb(PIC2_DATA) << 8) | inb(PIC1_DATA);  // read from DATA port
}
