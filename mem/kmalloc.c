// mem/kmalloc.c — size-aware allocator on top of getmem/freemem.

#include "kmalloc.h"
#include "memory.h"

#define KMALLOC_MAGIC  0xC0FFEE5050D15EA5UL  /* unlikely byte pattern  */

struct km_header {
    unsigned long size;     /* total bytes incl. header               */
    unsigned long magic;    /* sanity check on kfree                  */
    unsigned long reserved0; /* keep header 32 B = two cache lines/2  */
    unsigned long reserved1;
};

/* User pointer = header + 32 bytes; matches MEM_ALIGN=16. */
#define KM_HEADER_BYTES  (sizeof(struct km_header))

static unsigned long g_live_blocks;
static unsigned long g_live_bytes;
static unsigned long g_total_allocs;
static unsigned long g_total_frees;
static unsigned long g_bad_frees;

void *kmalloc(unsigned long nbytes)
{
    if (nbytes == 0) return 0;

    unsigned long total = nbytes + KM_HEADER_BYTES;
    struct km_header *h = (struct km_header *)getmem(total);
    if (h == 0) return 0;

    h->size      = total;
    h->magic     = KMALLOC_MAGIC;
    h->reserved0 = 0;
    h->reserved1 = 0;

    g_live_blocks++;
    g_live_bytes += total;
    g_total_allocs++;

    return (void *)((unsigned char *)h + KM_HEADER_BYTES);
}

void kfree(void *ptr)
{
    if (ptr == 0) return;

    struct km_header *h = (struct km_header *)((unsigned char *)ptr
                                                - KM_HEADER_BYTES);
    if (h->magic != KMALLOC_MAGIC) {
        /* Double-free, stray pointer, or stomped header — refuse to
         * feed it back to freemem, that would corrupt the free list. */
        g_bad_frees++;
        return;
    }

    unsigned long total = h->size;
    h->magic = 0;                  /* immediately invalidate */

    freemem((void *)h, total);

    if (g_live_blocks) g_live_blocks--;
    if (g_live_bytes >= total) g_live_bytes -= total;
    g_total_frees++;
}

unsigned long kmalloc_live_blocks(void)    { return g_live_blocks;    }
unsigned long kmalloc_live_bytes(void)     { return g_live_bytes;     }
unsigned long kmalloc_total_allocs(void)   { return g_total_allocs;   }
unsigned long kmalloc_total_frees(void)    { return g_total_frees;    }
unsigned long kmalloc_bad_frees(void)      { return g_bad_frees;      }
