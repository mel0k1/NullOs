#ifndef IDT_H
#define IDT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDT_SIZE 256

// Типы гейтов
#define IDT_INTERRUPT_GATE  0x8E
#define IDT_TRAP_GATE       0xEF
#define IDT_SYSTEM_GATE     0xEE

struct idt_entry {
    u16 offset_low;      // Биты 0-15 адреса
    u16 selector;        // Селектор сегмента кода
    u8  ist;             // IST (Interrupt Stack Table) - биты 0-2, нули в битах 3-7
    u8  type_attr;       // Тип и атрибуты
    u16 offset_mid;      // Биты 16-31 адреса
    u32 offset_high;     // Биты 32-63 адреса
    u32 reserved;        // Зарезервировано
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

// Глобальная таблица IDT
extern struct idt_entry idt[IDT_SIZE];
extern struct idt_ptr idtp;

// Функции инициализации
void idt_init();
void idt_set_gate(u8 num, u64 base, u16 sel, u8 flags);
void idt_set_gate_ist(u8 num, u64 base, u16 sel, u8 flags, u8 ist);

// Обработчики прерываний
void isr_handler(registers_t* regs);
void irq_handler(registers_t* regs);

#ifdef __cplusplus
}
#endif

#endif // IDT_H
