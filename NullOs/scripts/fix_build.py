import sys

p = 'kernel/proc.c'
lines = open(p, encoding='utf-8').readlines()

# Replace lines 135-258 (0-indexed: 134-257) with new implementation
new_func = r'''static bool build_user_image(u64 pml4_phys, const void* data, u64 size,
                             char* const argv[], int argc,
                             u64* out_rip, u64* out_rsp) {
    elf_load_info_t info;
    if (!elf_parse(data, size, &info) || !info.valid) return false;

    u64 load_base = ELF_USER_LOAD_BASE;
    if (info.base_address == load_base) load_base = 0;

    /* ---- Phase 1: allocate + fill PHYSICAL frames via identity ----
     * No CR3 switch needed: writes go through PHYS_TO_VIRT(pa),
     * which is always identity <4GB and always valid in kernel CR3.
     * This eliminates ALL demand faults and cross-CR3 issues.       */

    #define MAX_IMG_SEGS 16
    struct { u64 va, pa; u64 npages; u64 flags; } seg[MAX_IMG_SEGS];
    u32 nseg = 0;

    for (u16 i = 0; i < info.segment_count && nseg < MAX_IMG_SEGS; i++) {
        u64 dest   = info.segments[i].vaddr + load_base;
        u64 memsz  = info.segments[i].memsz;
        u64 filesz = info.segments[i].filesz;
        if (memsz == 0) continue;

        u64 page_start = PAGE_ALIGN(dest);
        u64 page_end   = PAGE_ROUND_UP(dest + memsz);
        u64 np         = (page_end - page_start) / PAGE_SIZE;
        u32 fl = VMM_PRESENT | VMM_USER;
        if (info.segments[i].flags & 2) fl |= VMM_WRITE;
        if (!(info.segments[i].flags & 1)) fl |= VMM_NX;

        phys_addr_t base_pa = pmm_alloc_pages(np);
        if (!base_pa) return false;
        kmemset((void*)PHYS_TO_VIRT(base_pa), 0, np * PAGE_SIZE);

        /* copy file data: source offset within file = vaddr - base_vaddr
         * for typical static ELFs where first LOAD starts at offset 0 */
        u64 src_off = info.segments[i].vaddr - info.base_address;
        if (filesz > 0 && src_off + filesz <= size)
            kmemcpy((void*)PHYS_TO_VIRT(base_pa),
                    (const u8*)data + src_off, filesz);

        seg[nseg].va     = page_start;
        seg[nseg].pa     = base_pa;
        seg[nseg].npages = np;
        seg[nseg].flags  = fl;
        nseg++;
    }

    /* ---- Stack: contiguous frames ---- */
    u64 stack_top = 0x7FFFFF000ULL;
    u64 stack_bot = stack_top - ELF_USER_STACK_SIZE;
    u64 stack_np  = ELF_USER_STACK_SIZE / PAGE_SIZE;
    phys_addr_t stk_pa = pmm_alloc_pages(stack_np);
    if (!stk_pa) return false;
    kmemset((void*)PHYS_TO_VIRT(stk_pa), 0, ELF_USER_STACK_SIZE);

    /* Build SysV initial stack DIRECTLY in the stack frames */
    static const char* def_argv[1] = { "/bin/program" };
    const char** sarg = (argv && argv[0]) ? (const char**)argv : def_argv;
    if (argc < 1) argc = 1;

    u64 str_bytes = 0;
    for (int i = 0; i < argc; i++) str_bytes += kstrlen(sarg[i]) + 1;
    str_bytes = (str_bytes + 15) & ~15ULL;
    u64 auxv_pairs = 11;
    u64 vec_total = str_bytes + (1 + (u64)argc + 1 + 1 + auxv_pairs*2)*8 + 16;
    if (vec_total > ELF_USER_STACK_SIZE / 2) return false;

    u8* stk = (u8*)PHYS_TO_VIRT(stk_pa);
    u64 str_start_off = ELF_USER_STACK_SIZE - str_bytes;
    u64 arg_va[16];
    int n = (argc > 16) ? 16 : argc;
    u64 soff = str_start_off;
    for (int i = 0; i < n; i++) {
        u64 len = kstrlen(sarg[i]) + 1;
        kmemcpy(stk + soff, sarg[i], len);
        arg_va[i] = stack_bot + soff;
        soff += len;
    }

    u64 vec_off = (soff + 15) & ~15ULL;
    u64* uv = (u64*)(stk + vec_off);
    uv[0] = (u64)n;
    for (int i = 0; i < n; i++) uv[1+i] = arg_va[i];
    uv[1+n] = 0; uv[2+n] = 0;

    /* auxv entries */
    u64 phdr_va = load_base > 0
        ? load_base + info.phoff
        : info.segments[0].vaddr + info.phoff;

    u64 rnd_off = vec_off + (3+(u64)n+1+1+auxv_pairs*2)*8;
    u64 t = timer_get_ticks();
    for (int k = 0; k < 16; k++) stk[rnd_off+k] = (u8)(t >> ((k&7)*8));
    u64 rnd_va = stack_bot + rnd_off;

    u64* av = (u64*)(stk + vec_off + (3+(u64)n+1+1)*8);
    *av++ = 3;  *av++ = phdr_va;
    *av++ = 4;  *av++ = info.phentsize;
    *av++ = 5;  *av++ = info.phnum;
    *av++ = 6;  *av++ = 4096;
    *av++ = 9;  *av++ = info.entry_point + load_base;
    *av++ = 11; *av++ = 0;
    *av++ = 12; *av++ = 0;
    *av++ = 13; *av++ = 0;
    *av++ = 14; *av++ = 0;
    *av++ = 23; *av++ = 0;
    *av++ = 25; *av++ = rnd_va;
    *av++ = 0;  *av++ = 0;

    u64 user_rsp = stack_bot + vec_off;

    /* ---- Phase 2: map everything into child PML4 ------------------ */
    for (u32 s = 0; s < nseg; s++) {
        for (u64 k = 0; k < seg[s].npages; k++) {
            u64 va = seg[s].va + k * PAGE_SIZE;
            u64 pa = seg[s].pa + k * PAGE_SIZE;
            u64* pte = vmm_walk_leaf(pml4_phys, va, true,
                                     !!(seg[s].flags & VMM_USER));
            if (!pte) return false;
            *pte = pa | seg[s].flags;
        }
    }
    for (u64 k = 0; k < stack_np; k++) {
        u64* pte = vmm_walk_leaf(pml4_phys, stack_bot + k * PAGE_SIZE,
                                 true, true);
        if (!pte) return false;
        *pte = (stk_pa + k * PAGE_SIZE)
               | VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX;
    }

    *out_rip = info.entry_point + load_base;
    *out_rsp = user_rsp;
    return true;
}
'''

result = ''.join(lines[:134]) + new_func + '\n' + ''.join(lines[258:])
open(p, 'w', encoding='utf-8').write(result)
print('replaced build_user_image: lines 135-258 -> new implementation')
