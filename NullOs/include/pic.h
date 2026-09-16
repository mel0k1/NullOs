#ifndef PIC_H
#define PIC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Порты PIC
#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

// Команды PIC
#define PIC_EOI         0x20    // End of Interrupt

// IRQ смещения (переназначаем на 32-47)
#define IRQ_OFFSET      32

// Маски IRQ
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_CASCADE     2
#define IRQ_COM2        3
#define IRQ_COM1        4
#define IRQ_LPT2        5
#define IRQ_FLOPPY      6
#define IRQ_LPT1        7
#define IRQ_RTC         8
#define IRQ_FREE1       9
#define IRQ_FREE2       10
#define IRQ_FREE3       11
#define IRQ_MOUSE       12
#define IRQ_FPU         13
#define IRQ_PRIMARY_ATA 14
#define IRQ_SECONDARY_ATA 15

// Функции инициализации и управления
void pic_init();
void pic_send_eoi(u8 irq);
void pic_set_mask(u8 irq);
void pic_unset_mask(u8 irq);
u16 pic_get_irr();
u16 pic_get_isr();

#ifdef __cplusplus
}
#endif

#endif // PIC_H
