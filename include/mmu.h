// include/mmu.h — AArch64 MMU / virtual-memory bring-up.

#ifndef XINU_RPI4_MMU_H
#define XINU_RPI4_MMU_H

/* Build an identity-mapped page table (RAM = Normal cacheable, everything
 * else = Device-nGnRnE) and turn on the MMU + D/I caches.  Call once,
 * early in kernel_main (after the exception vectors are installed so a
 * misconfiguration surfaces as a reported fault rather than a silent
 * hang).  Returns with virtual memory active and identity-mapped, so all
 * existing pointers stay valid. */
void mmu_init(void);

/* 1 once mmu_init() has enabled translation. */
int mmu_enabled(void);

#endif /* XINU_RPI4_MMU_H */
