#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/timer.h"
#include "../include/rtc.h"
#include "../include/scheduler.h"
#include "../include/string.h"
#include "../include/mouse.h"
#include "../include/serial.h"

// Глобальная таблица IDT
struct idt_entry idt[IDT_SIZE];
struct idt_ptr idtp;

// Объявления внешних обработчиков из assembly
extern void isr0(); extern void isr1(); extern void isr2(); extern void isr3();
extern void isr4(); extern void isr5(); extern void isr6(); extern void isr7();
extern void isr8(); extern void isr9(); extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); extern void isr15();
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
extern void isr20(); extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26(); extern void isr27();
extern void isr28(); extern void isr29(); extern void isr30(); extern void isr31();

extern void irq0(); extern void irq1(); extern void irq2(); extern void irq3();
extern void irq4(); extern void irq5(); extern void irq6(); extern void irq7();
extern void irq8(); extern void irq9(); extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

void idt_set_gate(u8 num, u64 base, u16 sel, u8 flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;  // IST = 0, используем обычный стек
    idt[num].type_attr = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void idt_set_gate_ist(u8 num, u64 base, u16 sel, u8 flags, u8 ist) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = ist & 0x07;  // IST: биты 0-2
    idt[num].type_attr = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void idt_init() {
    // Инициализируем указатель IDT
    idtp.limit = (sizeof(struct idt_entry) * IDT_SIZE) - 1;
    idtp.base = (u64)&idt;

    // Очищаем IDT
    for (int i = 0; i < IDT_SIZE; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Устанавливаем обработчики исключений (ISRs 0-31)
    idt_set_gate(0, (u64)isr0, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(1, (u64)isr1, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(2, (u64)isr2, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(3, (u64)isr3, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(4, (u64)isr4, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(5, (u64)isr5, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(6, (u64)isr6, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(7, (u64)isr7, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate_ist(8, (u64)isr8, 0x08, IDT_INTERRUPT_GATE, 1);  // #DF: IST1 (отдельный стек)
    idt_set_gate(9, (u64)isr9, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(10, (u64)isr10, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(11, (u64)isr11, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(12, (u64)isr12, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(13, (u64)isr13, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(14, (u64)isr14, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(15, (u64)isr15, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(16, (u64)isr16, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(17, (u64)isr17, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(18, (u64)isr18, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(19, (u64)isr19, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(20, (u64)isr20, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(21, (u64)isr21, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(22, (u64)isr22, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(23, (u64)isr23, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(24, (u64)isr24, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(25, (u64)isr25, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(26, (u64)isr26, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(27, (u64)isr27, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(28, (u64)isr28, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(29, (u64)isr29, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(30, (u64)isr30, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(31, (u64)isr31, 0x08, IDT_INTERRUPT_GATE);

    // Устанавливаем обработчики IRQ (32-47)
    idt_set_gate(32, (u64)irq0, 0x08, IDT_INTERRUPT_GATE);   // Timer
    idt_set_gate(33, (u64)irq1, 0x08, IDT_INTERRUPT_GATE);   // Keyboard
    idt_set_gate(34, (u64)irq2, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(35, (u64)irq3, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(36, (u64)irq4, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(37, (u64)irq5, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(38, (u64)irq6, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(39, (u64)irq7, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(40, (u64)irq8, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(41, (u64)irq9, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(42, (u64)irq10, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(43, (u64)irq11, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(44, (u64)irq12, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(45, (u64)irq13, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(46, (u64)irq14, 0x08, IDT_INTERRUPT_GATE);
    idt_set_gate(47, (u64)irq15, 0x08, IDT_INTERRUPT_GATE);

    // Загружаем IDT
    lidt(&idtp);
}

const char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Security Exception",
    "Reserved"
};

#include "../include/mm.h"

void page_fault_handler(registers_t* regs);

/* #pf-forensics v2 (see below): shared by user-write faults (Case 0)
 * and the pf_halt path for ANY user fault (read/np/etc). Runs once
 * per fault — pf_dump_done latch guards the double call. */
static bool pf_dump_done = false;

static void pf_user_forensics(registers_t* regs, u64 cr2) {
    /* #pf-forensics v2: ring-3 fault forensics. ALL physical reads
     * below happen inside a kernel-CR3 window (#pfdump-alias fix): the
     * old version read "PA" through the ACTIVE user space, whose
     * split low windows alias user VAs — that faked the 0x683000
     * "frame dup". Also scans the stack for code pointers so the
     * CALLER of the faulting frame shows up offline, dumps the CODE
     * page at RIP (detects executed-garbage), and tags the last
     * syscall for attribution. */
    extern u64* vmm_walk_leaf(phys_addr_t pml4_phys, u64 virt,
                              bool create_pt, bool user);
    extern phys_addr_t vmm_get_active_pml4(void);
    extern u64 vmm_pa_read_begin(void);
    extern void vmm_pa_read_end(u64);
    extern volatile u64 g_last_sc_nr, g_last_sc_pid;
    phys_addr_t ap = vmm_get_active_pml4();
    serial_printf("[PFDUMP] cr2=%lx rip=%lx cs=%lx lastsc=%lu/%lu\n",
                  cr2, regs->rip, regs->cs,
                  (unsigned long)g_last_sc_pid,
                  (unsigned long)g_last_sc_nr);
    serial_printf("[PFDUMP] r12=%lx r13=%lx r14=%lx r15=%lx\n",
                  regs->r12, regs->r13, regs->r14, regs->r15);
    serial_printf("[PFDUMP] r8=%lx r9=%lx r10=%lx r11=%lx\n",
                  regs->r8, regs->r9, regs->r10, regs->r11);
    serial_printf("[PFDUMP] rdi=%lx rsi=%lx rdx=%lx rcx=%lx rax=%lx\n",
                  regs->rdi, regs->rsi, regs->rdx, regs->rcx,
                  regs->rax);
    serial_printf("[PFDUMP] rbx=%lx rbp=%lx rsp=%lx\n",
                  regs->rbx, regs->rbp, regs->rsp);

    u64 saved_cr3 = vmm_pa_read_begin();   /* TRUE-PA reads below */

    /* faulting VA */
    u64* pte = vmm_walk_leaf(ap, cr2, false, false);
    if (pte) {
        u64 pa = *pte & 0x000ffffffffff000ULL;
        serial_printf("[PFDUMP] pte=%lx pa=%lx flags=[%s%s%s%s%s]\n",
                      *pte, pa,
                      (*pte & 1) ? "P" : "-",
                      (*pte & 2) ? "W" : "-",
                      (*pte & 4) ? "U" : "-",
                      (*pte & 0x200) ? "COW" : "-",
                      (*pte & (1ULL << 63)) ? "NX" : "-");
        const u64* pv = (const u64*)PHYS_TO_VIRT(pa);
        for (u32 q = 0; q < 4; q++)
            serial_printf("[PFDUMP] page+%x = %lx\n",
                          q * 8, pv[q]);
    } else {
        serial_printf("[PFDUMP] pte walk failed (cr2)\n");
    }

    /* CODE page at RIP: PTE + bytes. If these differ from the file's
     * bytes at the same offset, the executed text is corrupt. */
    {
        u64* rpt = vmm_walk_leaf(ap, regs->rip, false, false);
        if (rpt && (*rpt & 1)) {
            u64 rpa = *rpt & 0x000ffffffffff000ULL;
            serial_printf("[PFDUMP] rippte=%lx rippa=%lx\n", *rpt, rpa);
            const u64* rv = (const u64*)PHYS_TO_VIRT(
                rpa | (regs->rip & 0xFFFULL & ~7ULL));
            for (u32 q = 0; q < 4; q++)
                serial_printf("[PFDUMP] code+%x = %lx\n",
                              q * 8, rv[q]);
        } else {
            serial_printf("[PFDUMP] rip page not present!\n");
        }
    }

    /* user-VA read helper: walk active tables, read via the
     * (now kernel) identity mapping — no alias hazard */
    #define PFRD(va_, out_) do {                                  \
        u64* p_ = vmm_walk_leaf(ap, (va_) & ~7ULL, false, false); \
        if (p_ && (*p_ & 1))                                      \
            (out_) = *(volatile u64*)PHYS_TO_VIRT(                \
                (*p_ & 0x000ffffffffff000ULL) | ((va_) & 7ULL));  \
        else (out_) = 0xDEADBEEFDEADBEEFULL;                      \
    } while (0)

    /* wide stack window around rsp: 64 qwords, rsp-0x80 up */
    for (u32 k = 0; k < 64; k++) {
        s64 off = (s64)k * 8 - 0x80;
        u64 va = (u64)((s64)regs->rsp + off);
        u64 v; PFRD(va, v);
        if (v != 0xDEADBEEFDEADBEEFULL)
            serial_printf("[PFDUMP] stk%+04x = %lx\n",
                          (int)off, v);
    }
    /* scan a wider stack band for .text/.rodata/.data pointers
     * (return-address candidates of the faulting frame) */
    for (u64 va = regs->rsp - 0x400; va < regs->rsp + 0x400; va += 8) {
        u64 v; PFRD(va, v);
        if (v >= 0x400000ULL && v < 0x830000ULL &&
            v != 0xDEADBEEFDEADBEEFULL)
            serial_printf("[PFDUMP] txtscan va=%lx -> %lx\n", va, v);
    }
    /* deref the anchors doapr/vfprintf style: r14,rbx (buf ptrs),
     * rbp (currlen ptr), r15 (limit ptr) */
    {
        u64 anchors[4] = { regs->r14, regs->rbx, regs->rbp,
                           regs->r15 };
        const char* an[4] = { "r14", "rbx", "rbp", "r15" };
        for (u32 a = 0; a < 4; a++) {
            if (anchors[a] < 0x1000 ||
                anchors[a] >= 0x800000000000ULL) continue;
            u64 v; PFRD(anchors[a], v);
            serial_printf("[PFDUMP] deref %s(%lx) = %lx\n",
                          an[a], anchors[a], v);
        }
    }
    #undef PFRD
    vmm_pa_read_end(saved_cr3);
}

// Page fault error code bits
#define PF_PRESENT  (1 << 0)   // 0=not-present, 1=protection fault
#define PF_WRITE    (1 << 1)   // 0=read, 1=write
#define PF_USER     (1 << 2)   // 0=supervisor, 1=user-mode
#define PF_RESERVED (1 << 3)   // reserved bit set in page table
#define PF_ID       (1 << 4)   // 1=caused by instruction fetch

void page_fault_handler(registers_t* regs) {
    // Read CR2 to get the faulting address
    u64 cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    bool present   = (regs->err_code & PF_PRESENT) != 0;
    bool write     = (regs->err_code & PF_WRITE) != 0;
    bool user      = (regs->err_code & PF_USER) != 0;
    bool reserved  = (regs->err_code & PF_RESERVED) != 0;
    bool id_fetch  = (regs->err_code & PF_ID) != 0;

    // Case 0: copy-on-write fault (fork-shared page written)
    if (present && write && !reserved) {
        extern bool vmm_handle_cow(u64 virt);
        if (vmm_handle_cow(cr2)) {
            return;                       /* resolved: retry instruction */
        }
        if (user) {
            pf_dump_done = true;
            pf_user_forensics(regs, cr2);
        }
    }


    // Case 1: Not-present page — attempt demand paging
    // Only do demand paging for kernel addresses (< 128MB) that fall within
    // our identity-mapped region, OR for user-space addresses (>= 0x400000)
    if (!present && !reserved) {
        bool in_kernel_region = (cr2 < 128 * 1024 * 1024ULL);
        bool in_user_region   = (cr2 >= 0x400000ULL && cr2 < 0x800000000000ULL);

        // Security: a fault from ring 3 inside the kernel's identity-mapped
        // region must never be satisfied — otherwise user code could have
        // kernel memory mapped as USER+RW on demand (privilege escalation).
        if (user && in_kernel_region) {
            goto pf_halt;
        }

        if (in_kernel_region || in_user_region) {
            // Allocate a new physical page
            phys_addr_t phys = pmm_alloc_page();
            if (phys == 0 || phys < PAGE_SIZE) {
                extern u64* pmm_debug_bitmap(void);
                extern size_t pmm_debug_bitmap_words(void);
                u64* bm = pmm_debug_bitmap();
                serial_printf("[PF-OOM] cr2=%lx avail=%lu active_cr3=%lx\n",
                              cr2,
                              (unsigned long)pmm_get_available_memory(),
                              (unsigned long)vmm_get_active_pml4());
                serial_printf("[BM] ptr=%lx words=%lu w0=%lx w1=%lx last=%lx\n",
                              (unsigned long)(u64)bm,
                              (unsigned long)pmm_debug_bitmap_words(),
                              bm ? (unsigned long)bm[0] : 0UL,
                              bm ? (unsigned long)bm[1] : 0UL,
                              (bm && pmm_debug_bitmap_words())
                                ? (unsigned long)bm[pmm_debug_bitmap_words()-1]
                                : 0UL);
                vga_print("\n!!! PAGE FAULT: Out of memory !!!\n");
                goto pf_halt;
            }

            // Determine flags. VMM_USER is only granted for genuine
            // user-mode faults outside the kernel region.
            u64 flags = VMM_PRESENT | VMM_WRITE;
            if (user && !in_kernel_region) {
                flags |= VMM_USER;
            }
            if (id_fetch) {
                // Instruction fetch — mark executable (clear NX)
                // But kernel instruction fetch from user address = bad
                if (!user && in_user_region) {
                    vga_print("\n!!! PAGE FAULT: Kernel instruction fetch from user addr !!!\n");
                    goto pf_halt;
                }
            } else {
                flags |= VMM_NX;  // data pages: no execute
            }

            // Map the page (vmm_map auto-splits 2MB pages if needed)
            vmm_map(PAGE_ALIGN(cr2), phys, flags);

            // Zero the physical page for safety
            kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);

            // Page is now mapped — return and retry the faulting instruction
            return;
        }
        // Address outside valid regions — fall through to halt
    }

    // Case 2: Protection fault (present=1), reserved bit set, or bad address
    // These are genuine errors

pf_halt:
    /* #pf-kill-task: a ring-3 fault that demand-paging/COW could not
     * resolve (RO write via mprotect, exec of NX, kernel-VA touch,
     * OOM) must kill the FAULTING TASK, not the whole machine. Linux
     * delivers SIGSEGV here; we record exit_code=11 so wait4() reports
     * a signaled child. Kernel-mode faults still halt (kernel state is
     * untrustworthy). proc_exit_current switches away and never
     * returns — the interrupt frame on this task's kernel stack is
     * abandoned exactly like the exit(2) syscall path does.         */
    if (user) {
        if (!pf_dump_done) {
            pf_dump_done = true;
            pf_user_forensics(regs, cr2);
        }
        pf_dump_done = false;
        serial_printf("[PF-KILL] cr2=%lx rip=%lx cs=%lx err=%lx "
                      "(SIGSEGV)\n",
                      cr2, regs->rip, regs->cs, regs->err_code);
        vga_print("\n[SYS] Segmentation fault (task killed)\n");
        {
            extern void proc_exit_current(s32 code);
            task_t* t = task_get_current();
            if (t && t->pid > 0) proc_exit_current(11);
        }
        /* legacy pid<=0 context: no process layer to answer to —
         * historical halt below is the only safe answer.          */
    }
    if (user && !pf_dump_done) {
        pf_dump_done = true;
        pf_user_forensics(regs, cr2);
    }
    pf_dump_done = false;
    serial_printf("[PF-HALT] cr2=%lx rip=%lx cs=%lx err=%lx\n",
                  cr2, regs->rip, regs->cs, regs->err_code);
    vga_print("\n!!! EXCEPTION: ");
    vga_print(exception_messages[regs->int_num]);
    vga_print(" !!!\n");
    vga_print("Faulting address (CR2): 0x");
    vga_print_hex(cr2);
    vga_print("\nError Code: ");
    vga_print_hex(regs->err_code);
    vga_print(" (");
    if (present)  vga_print("P ");
    if (write)    vga_print("W ");
    if (user)     vga_print("U ");
    if (reserved) vga_print("RSV ");
    if (id_fetch) vga_print("ID ");
    vga_print(")");
    vga_print("\nRIP: ");
    vga_print_hex(regs->rip);
    vga_print("  CS: ");
    vga_print_hex(regs->cs);
    vga_print("\nRFLAGS: ");
    vga_print_hex(regs->rflags);
    vga_print("\nRAX: "); vga_print_hex(regs->rax);
    vga_print("  RBX: "); vga_print_hex(regs->rbx);
    vga_print("  RCX: "); vga_print_hex(regs->rcx);
    vga_print("\nRDX: "); vga_print_hex(regs->rdx);
    vga_print("  RSI: "); vga_print_hex(regs->rsi);
    vga_print("  RDI: "); vga_print_hex(regs->rdi);
    vga_print("\nRBP: "); vga_print_hex(regs->rbp);
    // Stack frame from regs upward (each field is u64):
    //   [0..15]  r15..rax (16 GPRs pushed by stub)
    //   [16]     int_num
    //   [17]     err_code
    //   [18]     rip  (CPU)
    //   [19]     cs   (CPU)
    //   [20]     rflags (CPU)
    //   [21]     rsp  (CPU, only on privilege change) = regs->rsp
    //   [22]     ss   (CPU, only on privilege change) = regs->ss
    vga_print("\nRSP: ");
    if (user) {
        // User mode: CPU pushed RSP/SS, accessible via regs->rsp
        vga_print_hex(regs->rsp);
        vga_print("  SS: ");
        vga_print_hex(regs->ss);
    } else if (regs->int_num != 8) {
        // Kernel mode, not IST: compute RSP from frame address
        // regs->rsp points to the address right after rflags on stack
        u64 fault_rsp = (u64)((u8*)regs + 20 * sizeof(u64));
        vga_print_hex(fault_rsp);
    } else {
        vga_print("(on IST1 - original lost)");
    }
    vga_print("\n\nSystem halted.\n");

    while(1) { hlt(); }
}

void isr_handler(registers_t* regs) {
    if (regs->int_num < 32) {
        // Page fault has special handler with demand paging
        if (regs->int_num == 14) {
            page_fault_handler(regs);
            return;
        }

        // All other exceptions: print dump and halt
        vga_print("\n!!! EXCEPTION: ");
        vga_print(exception_messages[regs->int_num]);
        vga_print(" !!!\n");
        vga_print("Error Code: ");
        vga_print_hex(regs->err_code);
        vga_print("\nRIP: ");
        vga_print_hex(regs->rip);
        vga_print("  CS: ");
        vga_print_hex(regs->cs);
        vga_print("\nRFLAGS: ");
        vga_print_hex(regs->rflags);
        vga_print("\nRAX: "); vga_print_hex(regs->rax);
        vga_print("  RBX: "); vga_print_hex(regs->rbx);
        vga_print("  RCX: "); vga_print_hex(regs->rcx);
        vga_print("\nRDX: "); vga_print_hex(regs->rdx);
        vga_print("  RSI: "); vga_print_hex(regs->rsi);
        vga_print("  RDI: "); vga_print_hex(regs->rdi);
        vga_print("\nRBP: "); vga_print_hex(regs->rbp);
        vga_print("  RSP: (on IST/exception stack)\n");
        {
            /* Real faulting context lives in the CPU-pushed frame even
             * though we execute on IST: cs&3==3 => RSP/SS are valid
             * fields of the interrupt frame right after RFLAGS. */
            bool um = (regs->cs & 3) == 3;
            serial_printf("[EXC] num=%lu err=%lx rip=%lx cs=%lx "
                          "usrsp=%lx sssp=%lx\n",
                          (unsigned long)regs->int_num,
                          (unsigned long)regs->err_code,
                          (unsigned long)regs->rip,
                          (unsigned long)regs->cs,
                          (unsigned long)(um ? regs->rsp : 0),
                          (unsigned long)(um ? regs->ss : 0));
            if (um) {
                vga_print("User RSP: ");
                vga_print_hex(regs->rsp);
                vga_print(" SS: ");
                vga_print_hex(regs->ss);
                vga_print("\n");
            }
        }
        vga_print("\nSystem halted.\n");

        /* #apk-exit-malcheck forensics: a USER-mode #GP is musl's
         * a_crash() (mallocng metadata trap compiled as hlt).
         *
         * Crash site disassembly (apk.static 0x6e372c) pins the live
         * registers: r9 = p (freed user pointer), rdx = end (slot end),
         * rsi = end-p, rax = reserved. The trap that fires there is
         * get_nominal_size(): assert(reserved <= end-p) — i.e. the
         * chunk's trailing-reserved field claims more bytes than the
         * slot holds, the signature of a header clobbered after malloc.
         *
         * Dump: the chunk header (p-8..p+23), the slot tail (end-16..
         * end+7, covers the u32 reserved at end-4 and the end[-1]/end[0]
         * guard bytes), and the group/meta structs reconstructed the way
         * mallocng's get_meta does it (base = p - 16*offset - 16).
         * All reads are user VAs under the still-active faulting CR3,
         * page-guarded so an unmapped pointer cannot double-fault. */
        if ((regs->cs & 3) == 3 && regs->int_num == 13) {
            u64 p = regs->r9;
            u64 end = regs->rdx;
            serial_printf("[GPDUMP2] p=%lx end=%lx end_p=%lx rax=%lx "
                          "rbx=%lx\n",
                          p, end, end - p, regs->rax, regs->rbx);
            extern u64 vmm_get_phys(u64 va);
            #define GPD_BYTE(va) ({ \
                u64 _va = (va); \
                u64 _ph = vmm_get_phys(_va & ~0xFFFUL); \
                (_ph != 0) ? *(volatile const u8*)_va : 0xAA; })
            serial_printf("[GPDUMP2] chunk-header @p-8..p+23:\n");
            for (int i = -8; i < 24; i++) {
                serial_printf(" %02x", GPD_BYTE(p + i));
                if (((i + 8) & 15) == 15) serial_printf("\n");
            }
            serial_printf("[GPDUMP2] slot-tail @end-16..end+7:\n");
            for (int i = -16; i < 8; i++) {
                serial_printf(" %02x", GPD_BYTE(end + i));
                if (((i + 16) & 15) == 15) serial_printf("\n");
            }
            #undef GPD_BYTE
            /* reconstruct group base like get_meta: offset = *(u16*)(p-2) */
            {
                extern u64 vmm_get_phys(u64 va);
                u64 off_pg = vmm_get_phys((p - 2) & ~0xFFFUL);
                if (p > 32 && off_pg != 0) {
                    u16 off = *(volatile const u16*)(p - 2);
                    u64 base = p - 16UL * off - 16;
                    u64 bpg = vmm_get_phys(base & ~0xFFFUL);
                    serial_printf("[GPDUMP2] off=%u base=%lx %s\n",
                                  off, base,
                                  bpg ? "(mapped)" : "(UNMAPPED!)");
                    if (bpg != 0) {
                        serial_printf("[GPDUMP2] group @base..+31:\n");
                        for (int i = 0; i < 32; i++) {
                            serial_printf(" %02x",
                                *(volatile const u8*)(base + i));
                            if ((i & 15) == 15) serial_printf("\n");
                        }
                        /* meta = *(void**)base (struct group.meta) */
                        u64 meta = *(volatile const u64*)base;
                        u64 mpg = vmm_get_phys(meta & ~0xFFFUL);
                        serial_printf("[GPDUMP2] meta=%lx %s\n",
                                      meta, mpg ? "(mapped)" : "(UNMAPPED!)");
                        if (mpg != 0) {
                            serial_printf("[GPDUMP2] meta-struct 48B:\n");
                            for (int i = 0; i < 48; i++) {
                                serial_printf(" %02x",
                                    *(volatile const u8*)(meta + i));
                                if ((i & 15) == 15) serial_printf("\n");
                            }
                        }
                    }
                }
            }
        }

        while(1) { hlt(); }
    }
}

void irq_handler(registers_t* regs) {
    u8 irq = regs->int_num - 32;

    // Handle specific IRQs
    if (irq == 0) {
        timer_handler();       // PIT timer tick
        scheduler_tick();      // Wake sleeping tasks

        /* True preemption: if the quantum expired and another task is
         * READY, switch away HERE. scheduler_preempt sends its own EOI
         * when it switches and never returns; otherwise we fall through
         * to the normal EOI below. */
        extern void scheduler_preempt(registers_t* regs);
        scheduler_preempt(regs);
    }
    if (irq == 1) {
        keyboard_handler();    // PS/2 keyboard
    }
    if (irq == 8) {
        rtc_handler();         // RTC periodic interrupt
    }
    if (irq == 12) {
        mouse_irq_handler();   // PS/2 Mouse
    }

    /* RTL8139 NIC (typically IRQ 11 in QEMU default PCI routing) */
    if (irq == 11) {
        extern void rtl8139_irq_handler(void);
        rtl8139_irq_handler();
    }

    // Send EOI to PIC
    pic_send_eoi(irq);
}
