// include/kmalloc.h — size-aware kmalloc / kfree.
//
// Layered on top of the first-fit getmem/freemem in mem/memory.c.
// The low-level API needs the caller to remember the allocation
// size (because the free list keeps no per-block metadata); this
// layer hides that with a 32-byte header in front of every block:
//
//   [ size | magic | (16 B padding to keep return ptr 16-aligned) ] [ user bytes ... ]
//   ^                                                                ^
//   what getmem returns                                              what kmalloc returns
//
// kfree() recovers the header from the user pointer, sanity-checks
// the magic, and feeds the original size back to freemem().

#ifndef XINU_RPI5_KMALLOC_H
#define XINU_RPI5_KMALLOC_H

/* Allocate `nbytes` of kernel memory.  Returns NULL on out-of-memory.
 * The returned pointer is 16-byte aligned (matching MEM_ALIGN in
 * memory.h) so the caller can safely place AArch64 SP frames there. */
void *kmalloc(unsigned long nbytes);

/* Free a block previously returned by kmalloc().  NULL is a no-op.
 * Wrong magic = double-free or stray pointer; we bump
 * kmalloc_bad_free and return without touching the heap. */
void  kfree(void *ptr);

/* Live counters — used by the Memory window in main.c. */
unsigned long kmalloc_live_blocks(void);
unsigned long kmalloc_live_bytes(void);
unsigned long kmalloc_total_allocs(void);
unsigned long kmalloc_total_frees(void);
unsigned long kmalloc_bad_frees(void);

#endif /* XINU_RPI5_KMALLOC_H */
