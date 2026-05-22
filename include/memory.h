// include/memory.h — xinu-rpi5 first-fit allocator interface.

#ifndef XINU_RPI5_MEMORY_H
#define XINU_RPI5_MEMORY_H

struct memblk {
    struct memblk *mnext;
    unsigned long  mlength;
};

void mem_init(unsigned long heap_start, unsigned long heap_end);
void *getmem(unsigned long nbytes);
void  freemem(void *block, unsigned long nbytes);
unsigned long mem_free_bytes(void);
unsigned long mem_total_bytes(void);
unsigned long mem_largest_block(void);
int           mem_free_block_count(void);

extern unsigned char _end[];

#endif /* XINU_RPI5_MEMORY_H */
