// include/cc.h — public interface to the on-device C compiler.

#ifndef XINU_RPI5_CC_H
#define XINU_RPI5_CC_H

/* `cc <file.c>` shell command. */
int cmd_cc(int argc, char **argv);

/* Compile C source (`src`, `srclen` bytes; need not be NUL-terminated)
 * and run it in place (JIT).  Program output from the builtins
 * print/putchar/puts is captured into `out` (NUL-terminated, capped at
 * `outcap`).  On success returns 0 and sets *retval to the program's
 * return value.  On compile error returns -1 and writes "cc: <message>"
 * into `out`.  Returns -2 on out-of-memory. */
int cc_run_source(const char *src, int srclen, char *out, int outcap, long *retval);

#endif /* XINU_RPI5_CC_H */
