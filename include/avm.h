// include/avm.h — AIPL actor-bytecode VM + "Blender" polygon display (device/video/avm.c).
#ifndef XINU_RPI4_AVM_H
#define XINU_RPI4_AVM_H

void avm_stage_reset(void);                                   /* begin a new upload */
int  avm_stage_put(int off, const unsigned char *b, int n);   /* stage a chunk      */
int  avm_loadrun(int len);                                     /* load + run staged  */

#endif /* XINU_RPI4_AVM_H */
