/*
 * dlmorecore.h — force-included into the vendored dlmalloc compile
 * (Makefile: `-include $(KERNEL_DIR)/dlmorecore.h`) so the pristine
 * source sees the kernel's MORECORE function without a single edit
 * to malloc.c.
 *
 * kernel_sbrk lives in kernel/mm.c (dlmalloc-backed heap section):
 * strict Unix sbrk(2) semantics over the fixed 64MB kernel arena.
 */
#ifndef _DLMORECORE_H
#define _DLMORECORE_H

void* kernel_sbrk(long inc);

#endif /* _DLMORECORE_H */
