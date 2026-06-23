// os_avm_stub.c — OS1/OS2 link shims for the blender display (avm.c), which those
// minimal kernels drop entirely.  The WM right-click menu, the /actor/loadvm HTTP
// route and main.c still reference these symbols; here they're no-ops so OS1/OS2
// link without avm.o.  kexec lives in system/kexec.c, independent of avm.c, so
// runtime OS switching still works.
#include "wm.h"

void        avm_stage_reset(void) {}
int         avm_stage_put(int off, const unsigned char *b, int n) { (void)off; (void)b; (void)n; return -1; }
void        avm_load_progress(int cur, int total) { (void)cur; (void)total; }
int         avm_loadrun(int len) { (void)len; return -1; }
int         avm_save(const char *name, int len) { (void)name; (void)len; return -1; }
int         avm_save_sd(const char *name, int len) { (void)name; (void)len; return -1; }
int         avm_sdread_diag(const char *name, int *first, int *size, int *got, int *faillba, int *magic)
{ (void)name; if (first) *first = 0; if (size) *size = 0; if (got) *got = 0;
  if (faillba) *faillba = -1; if (magic) { magic[0]=magic[1]=magic[2]=magic[3]=0; } return -1; }
void        avm_open_list(void) {}
void        avm_run_makina(void) {}
void        avm_draw_loadbar(int sw, int sh) { (void)sw; (void)sh; }
void        avm_key(char c) { (void)c; }
window_t   *avm_window(void) { return 0; }
