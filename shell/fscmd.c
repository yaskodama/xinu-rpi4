// shell/fscmd.c — hierarchical-filesystem shell commands.
//
// These implement the user-facing file commands (ls/cd/pwd/mkdir/cat/
// edit/write/rm/rmdir/tree/touch/cp/mv) on top of the tmpfs VFS in
// fs/vfs.c.  They are registered in shell/shell.c's commandtab.  All
// I/O is direct PL011 UART (no libc), matching the rest of the kernel.

#include "vfs.h"
#include "uart.h"
#include "kmalloc.h"
#include "shell.h"

/* ---------- tiny local helpers (house style: each file carries its own) ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static void puts_dec(unsigned long v)
{
    char buf[20];
    int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(buf[n]);
}

static void err2(const char *cmd, const char *msg, const char *arg)
{
    uart_puts(cmd); uart_puts(": "); uart_puts(msg);
    if (arg) { uart_puts(": "); uart_puts(arg); }
    uart_puts("\n");
}

/* Resolve `path` to a writable regular-file node, creating it if it does
 * not exist.  If `path` names an existing directory, the file `altname`
 * is created/used inside it.  Returns NULL on error. */
static vfs_node_t *dst_file(const char *path, const char *altname)
{
    vfs_node_t *n = vfs_resolve(path);
    if (n && n->kind == VFS_FILE) return n;

    vfs_node_t *dir;
    char leaf[VFS_NAME_MAX + 1];

    if (n && n->kind == VFS_DIR) {
        dir = n;
        int i = 0;
        while (altname[i] && i < VFS_NAME_MAX) { leaf[i] = altname[i]; i++; }
        leaf[i] = 0;
    } else {
        dir = vfs_resolve_parent(path, leaf, VFS_NAME_MAX + 1);
        if (!dir) return 0;
    }

    vfs_node_t *ex = vfs_child(dir, leaf);
    if (ex) return (ex->kind == VFS_FILE) ? ex : 0;
    return vfs_create_file(dir, leaf);
}

/* ---------- commands ---------- */

int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[256];
    if (vfs_path(vfs_cwd(), buf, sizeof buf) < 0) { err2("pwd", "path too long", 0); return -1; }
    uart_puts(buf); uart_puts("\n");
    return 0;
}

int cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : 0;
    vfs_node_t *n = path ? vfs_resolve(path) : vfs_cwd();
    if (!n) { err2("ls", "no such path", path); return -1; }

    if (n->kind == VFS_FILE) {
        uart_puts(n->name); uart_puts("  "); puts_dec(n->size); uart_puts(" bytes\n");
        return 0;
    }
    for (vfs_node_t *c = n->children; c; c = c->next) {
        uart_puts(c->name);
        if (c->kind == VFS_DIR) {
            uart_puts("/");
        } else {
            uart_puts("  ("); puts_dec(c->size); uart_puts("B)");
        }
        uart_puts("\n");
    }
    return 0;
}

int cmd_cd(int argc, char **argv)
{
    vfs_node_t *n = (argc < 2) ? vfs_root() : vfs_resolve(argv[1]);
    if (!n) { err2("cd", "no such path", argv[1]); return -1; }
    if (vfs_chdir(n) != 0) { err2("cd", "not a directory", argv[1]); return -1; }
    return 0;
}

int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: mkdir <path>\n"); return -1; }
    char leaf[VFS_NAME_MAX + 1];
    vfs_node_t *parent = vfs_resolve_parent(argv[1], leaf, sizeof leaf);
    if (!parent) { err2("mkdir", "bad path", argv[1]); return -1; }
    if (!vfs_mkdir(parent, leaf)) { err2("mkdir", "exists or failed", argv[1]); return -1; }
    return 0;
}

int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: touch <path>\n"); return -1; }
    if (!dst_file(argv[1], 0)) { err2("touch", "failed", argv[1]); return -1; }
    return 0;
}

int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: cat <path>...\n"); return -1; }
    for (int a = 1; a < argc; a++) {
        vfs_node_t *n = vfs_resolve(argv[a]);
        if (!n || n->kind != VFS_FILE) { err2("cat", "not a file", argv[a]); continue; }
        const char *d = (const char *)n->data;
        for (unsigned long i = 0; i < n->size; i++) uart_putc(d[i]);
        if (n->size && d[n->size - 1] != '\n') uart_putc('\n');
    }
    return 0;
}

int cmd_write(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: write <path> [text...]\n"); return -1; }
    vfs_node_t *f = dst_file(argv[1], 0);
    if (!f) { err2("write", "cannot create", argv[1]); return -1; }

    char buf[SHELL_BUFLEN + 1];
    int len = 0;
    for (int a = 2; a < argc; a++) {
        for (char *s = argv[a]; *s && len < SHELL_BUFLEN; s++) buf[len++] = *s;
        if (a + 1 < argc && len < SHELL_BUFLEN) buf[len++] = ' ';
    }
    if (len < SHELL_BUFLEN) buf[len++] = '\n';
    vfs_write(f, buf, (unsigned long)len);
    return 0;
}

/* Multi-line editor: appends typed lines to the file until a line that
 * is just ".".  Works interactively (UART shell) and when commands are
 * piped on stdin (qemu-smoke).  Overwrites any existing contents. */
int cmd_edit(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: edit <path>   (end with a single '.')\n"); return -1; }
    vfs_node_t *f = dst_file(argv[1], 0);
    if (!f) { err2("edit", "cannot create", argv[1]); return -1; }

    unsigned long cap = 8192;
    char *acc = (char *)kmalloc(cap);
    if (!acc) { err2("edit", "out of memory", 0); return -1; }
    unsigned long len = 0;

    uart_puts("// type text; a single '.' on its own line ends input\n");
    char line[SHELL_BUFLEN];
    for (;;) {
        int k = uart_getline(line, SHELL_BUFLEN);
        if (k == 1 && line[0] == '.') break;
        for (int i = 0; i < k && len < cap - 1; i++) acc[len++] = line[i];
        if (len < cap - 1) acc[len++] = '\n';
    }
    vfs_write(f, acc, len);
    kfree(acc);
    uart_puts("saved "); puts_dec(len); uart_puts(" bytes\n");
    return 0;
}

int cmd_rm(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: rm <file>...\n"); return -1; }
    for (int a = 1; a < argc; a++) {
        vfs_node_t *n = vfs_resolve(argv[a]);
        if (!n) { err2("rm", "no such file", argv[a]); continue; }
        if (n->kind != VFS_FILE) { err2("rm", "not a file (use rmdir)", argv[a]); continue; }
        if (vfs_unlink(n) != 0) err2("rm", "failed", argv[a]);
    }
    return 0;
}

int cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: rmdir <dir>\n"); return -1; }
    vfs_node_t *n = vfs_resolve(argv[1]);
    if (!n) { err2("rmdir", "no such dir", argv[1]); return -1; }
    if (n == vfs_cwd()) { err2("rmdir", "cannot remove the current directory", argv[1]); return -1; }
    if (vfs_rmdir(n) != 0) { err2("rmdir", "not empty / not a dir / root", argv[1]); return -1; }
    return 0;
}

static void tree_visit(int depth, vfs_node_t *n, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < depth; i++) uart_puts("  ");
    uart_puts(n->name[0] ? n->name : "/");
    if (n->kind == VFS_DIR && n->name[0]) uart_puts("/");
    uart_puts("\n");
}

int cmd_tree(int argc, char **argv)
{
    vfs_node_t *n = (argc >= 2) ? vfs_resolve(argv[1]) : vfs_cwd();
    if (!n) { err2("tree", "no such path", argv[1]); return -1; }
    vfs_walk(n, 0, tree_visit, 0);
    return 0;
}

int cmd_cp(int argc, char **argv)
{
    if (argc < 3) { uart_puts("usage: cp <src> <dst>\n"); return -1; }
    vfs_node_t *src = vfs_resolve(argv[1]);
    if (!src || src->kind != VFS_FILE) { err2("cp", "src not a file", argv[1]); return -1; }
    vfs_node_t *dst = dst_file(argv[2], src->name);
    if (!dst) { err2("cp", "cannot create dst", argv[2]); return -1; }
    if (dst == src) { err2("cp", "source and dest are the same", argv[1]); return -1; }
    if (vfs_write(dst, src->data, src->size) != 0) { err2("cp", "write failed", argv[2]); return -1; }
    return 0;
}

int cmd_mv(int argc, char **argv)
{
    if (argc < 3) { uart_puts("usage: mv <src-file> <dst>\n"); return -1; }
    vfs_node_t *src = vfs_resolve(argv[1]);
    if (!src || src->kind != VFS_FILE) { err2("mv", "src not a file", argv[1]); return -1; }
    vfs_node_t *dst = dst_file(argv[2], src->name);
    if (!dst) { err2("mv", "cannot create dst", argv[2]); return -1; }
    if (dst == src) return 0;
    if (vfs_write(dst, src->data, src->size) != 0) { err2("mv", "write failed", argv[2]); return -1; }
    vfs_unlink(src);
    return 0;
}

/* str_eq kept for potential future use; silence -Wunused. */
static inline void fscmd_unused(void) { (void)str_eq; }
