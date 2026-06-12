// fs/vfs.c — tmpfs-backed hierarchical filesystem.

#include "vfs.h"
#include "kmalloc.h"

static vfs_node_t g_root = {
    .name     = "",
    .kind     = VFS_DIR,
    .size     = 0,
    .capacity = 0,
    .data     = 0,
    .parent   = 0,
    .children = 0,
    .next     = 0,
};

static unsigned long g_node_count = 1;  /* root counts */

vfs_node_t *vfs_root(void) { return &g_root; }

unsigned long vfs_node_count(void) { return g_node_count; }

unsigned long vfs_total_file_bytes(void)
{
    /* Walk the tree summing every file's size.  Recursive; tree
     * depth in practice is small (< 8 in our demo). */
    unsigned long total = 0;

    /* Manual stack-based DFS would avoid recursion, but with tens of
     * nodes the C stack handles this easily.  */
    vfs_node_t *stack[64];
    int top = 0;
    stack[top++] = &g_root;
    while (top > 0) {
        vfs_node_t *n = stack[--top];
        if (n->kind == VFS_FILE) total += n->size;
        for (vfs_node_t *c = n->children; c && top < 64; c = c->next) {
            stack[top++] = c;
        }
    }
    return total;
}

/* ---------- helpers ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static int str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

static vfs_node_t *find_child(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    for (vfs_node_t *c = parent->children; c; c = c->next) {
        if (str_eq(c->name, name)) return c;
    }
    return 0;
}

static vfs_node_t *new_node(vfs_kind_t kind, const char *name,
                            vfs_node_t *parent)
{
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (n == 0) return 0;
    str_copy(n->name, name, VFS_NAME_MAX + 1);
    n->kind     = kind;
    n->size     = 0;
    n->capacity = 0;
    n->data     = 0;
    n->parent   = parent;
    n->children = 0;
    n->next     = 0;

    /* Tail-insert into parent's child list for stable iteration order. */
    if (parent) {
        if (parent->children == 0) {
            parent->children = n;
        } else {
            vfs_node_t *t = parent->children;
            while (t->next) t = t->next;
            t->next = n;
        }
    }
    g_node_count++;
    return n;
}

vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    if (find_child(parent, name)) return 0;            /* collision */
    return new_node(VFS_DIR, name, parent);
}

vfs_node_t *vfs_create_file(vfs_node_t *parent, const char *name)
{
    if (parent == 0 || parent->kind != VFS_DIR) return 0;
    if (find_child(parent, name)) return 0;
    return new_node(VFS_FILE, name, parent);
}

int vfs_write(vfs_node_t *file, const void *buf, unsigned long len)
{
    if (file == 0 || file->kind != VFS_FILE) return -1;

    /* Grow buffer if necessary; we don't shrink. */
    if (len > file->capacity) {
        void *nb = kmalloc(len);
        if (nb == 0) return -1;
        if (file->data) kfree(file->data);
        file->data     = nb;
        file->capacity = len;
    }

    const unsigned char *s = (const unsigned char *)buf;
    unsigned char       *d = (unsigned char *)file->data;
    for (unsigned long i = 0; i < len; i++) d[i] = s[i];
    file->size = len;
    return 0;
}

int vfs_read(vfs_node_t *file, void *buf, unsigned long max)
{
    if (file == 0 || file->kind != VFS_FILE) return -1;
    unsigned long n = file->size < max ? file->size : max;
    const unsigned char *s = (const unsigned char *)file->data;
    unsigned char       *d = (unsigned char *)buf;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return (int)n;
}

int vfs_write_str(vfs_node_t *file, const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return vfs_write(file, s, n);
}

vfs_node_t *vfs_lookup(const char *path)
{
    if (path == 0 || path[0] != '/') return 0;
    vfs_node_t *cur = &g_root;
    const char *p = path + 1;
    while (*p) {
        char name[VFS_NAME_MAX + 1];
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX) { name[i++] = *p++; }
        name[i] = 0;
        if (i > 0) {
            cur = find_child(cur, name);
            if (cur == 0) return 0;
        }
        if (*p == '/') p++;
    }
    return cur;
}

void vfs_walk(vfs_node_t *node, int depth,
              vfs_visit_fn visit, void *ctx)
{
    if (node == 0) return;
    visit(depth, node, ctx);
    for (vfs_node_t *c = node->children; c; c = c->next) {
        vfs_walk(c, depth + 1, visit, ctx);
    }
}

/* ---------- current working directory + path resolution ---------- */

static vfs_node_t *g_cwd = 0;

vfs_node_t *vfs_cwd(void)
{
    if (g_cwd == 0) g_cwd = &g_root;
    return g_cwd;
}

int vfs_chdir(vfs_node_t *dir)
{
    if (dir == 0 || dir->kind != VFS_DIR) return -1;
    g_cwd = dir;
    return 0;
}

/* Core walker shared by vfs_resolve / vfs_resolve_parent.  Walks the
 * path one component at a time.  When `want_parent` is set we stop just
 * before the final component, copy it into `leaf`, and return the
 * containing directory. */
static vfs_node_t *resolve_walk(const char *path, int want_parent,
                                char *leaf, int leafmax)
{
    const char *p = path ? path : "";
    vfs_node_t *cur = (*p == '/') ? &g_root : vfs_cwd();
    char name[VFS_NAME_MAX + 1];

    while (*p) {
        while (*p == '/') p++;               /* skip slash run */
        if (*p == 0) break;

        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX) name[i++] = *p++;
        if (*p && *p != '/') return 0;        /* component too long */
        name[i] = 0;

        const char *q = p;                    /* is this the last component? */
        while (*q == '/') q++;
        int is_last = (*q == 0);

        if (want_parent && is_last) {
            if (cur->kind != VFS_DIR) return 0;
            if (str_eq(name, ".") || str_eq(name, "..")) return 0;
            str_copy(leaf, name, leafmax);
            return cur;
        }

        if (str_eq(name, ".")) {
            /* stay */
        } else if (str_eq(name, "..")) {
            cur = cur->parent ? cur->parent : cur;
        } else {
            if (cur->kind != VFS_DIR) return 0;
            vfs_node_t *c = find_child(cur, name);
            if (c == 0) return 0;
            cur = c;
        }
        p = q;
    }

    if (want_parent) return 0;                /* no final component */
    return cur;
}

vfs_node_t *vfs_resolve(const char *path)
{
    if (path == 0 || path[0] == 0) return vfs_cwd();
    return resolve_walk(path, 0, 0, 0);
}

vfs_node_t *vfs_resolve_parent(const char *path, char *leaf, int leafmax)
{
    if (path == 0 || leaf == 0 || leafmax < 1) return 0;
    return resolve_walk(path, 1, leaf, leafmax);
}

int vfs_path(vfs_node_t *node, char *buf, int max)
{
    if (node == 0 || buf == 0 || max < 2) return -1;
    if (node == &g_root) { buf[0] = '/'; buf[1] = 0; return 1; }

    /* Collect ancestors leaf-first, then emit root-first. */
    vfs_node_t *stack[32];
    int top = 0;
    for (vfs_node_t *n = node; n && n != &g_root; n = n->parent) {
        if (top >= 32) return -1;
        stack[top++] = n;
    }

    int len = 0;
    while (top > 0) {
        vfs_node_t *n = stack[--top];
        if (len >= max - 1) return -1;
        buf[len++] = '/';
        for (int i = 0; n->name[i]; i++) {
            if (len >= max - 1) return -1;
            buf[len++] = n->name[i];
        }
    }
    buf[len] = 0;
    return len;
}

/* Unlink `node` from its parent's singly-linked child list. */
static int detach_child(vfs_node_t *node)
{
    vfs_node_t *parent = node->parent;
    if (parent == 0) return -1;
    vfs_node_t **pp = &parent->children;
    while (*pp && *pp != node) pp = &(*pp)->next;
    if (*pp != node) return -1;
    *pp = node->next;
    return 0;
}

int vfs_unlink(vfs_node_t *file)
{
    if (file == 0 || file->kind != VFS_FILE) return -1;
    if (detach_child(file) != 0) return -1;
    if (file->data) kfree(file->data);
    kfree(file);
    g_node_count--;
    return 0;
}

int vfs_rmdir(vfs_node_t *dir)
{
    if (dir == 0 || dir->kind != VFS_DIR) return -1;
    if (dir == &g_root) return -1;
    if (dir->children) return -1;             /* not empty */
    if (detach_child(dir) != 0) return -1;
    kfree(dir);
    g_node_count--;
    return 0;
}

vfs_node_t *vfs_child(vfs_node_t *dir, const char *name)
{
    return find_child(dir, name);
}
