// include/vfs.h — minimal hierarchical in-memory file system.
//
// Single global namespace rooted at "/".  Each node is either a
// directory (containing child nodes) or a regular file (containing a
// byte payload allocated via kmalloc).  All paths are absolute strings
// of the form "/dir/dir/.../file" — no relative paths, no current
// working directory.  No mount points, no symlinks, no hard links.
//
// Storage backend = tmpfs only: everything lives in RAM and is lost
// at reboot.  Persistent backends (FAT32 on the SD card, ext2/ext4
// on the Pi OS rootfs partition) can be added later as alternative
// implementations of the same vfs_node_t interface.

#ifndef XINU_RPI4_VFS_H
#define XINU_RPI4_VFS_H

#define VFS_NAME_MAX  31

typedef enum {
    VFS_DIR,
    VFS_FILE
} vfs_kind_t;

typedef struct vfs_node {
    char           name[VFS_NAME_MAX + 1];
    vfs_kind_t     kind;
    unsigned long  size;       /* bytes (files) or 0 (dirs)         */
    unsigned long  capacity;   /* allocated bytes in `data` buffer  */
    void          *data;       /* file payload                      */
    struct vfs_node *parent;
    struct vfs_node *children; /* head of child list  (dirs only)   */
    struct vfs_node *next;     /* sibling link                       */
} vfs_node_t;

/* The implicit root "/" node.  Statically allocated; never freed. */
vfs_node_t *vfs_root(void);

/* Create a child directory / file under `parent`.  Returns the new
 * node, or NULL on OOM / name collision / invalid parent. */
vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_create_file(vfs_node_t *parent, const char *name);

/* Write/read raw bytes to/from a regular file.  vfs_write replaces
 * the existing contents (the file is grown via kmalloc as needed).
 * vfs_read copies up to `max` bytes from offset 0 into `buf` and
 * returns the number of bytes actually transferred. */
int vfs_write(vfs_node_t *file, const void *buf, unsigned long len);
int vfs_read (vfs_node_t *file, void *buf, unsigned long max);

/* Convenience: write the C string `s` (without trailing NUL) into
 * `file`, replacing existing contents.  Returns 0 / -1. */
int vfs_write_str(vfs_node_t *file, const char *s);

/* Resolve an absolute path "/foo/bar".  Returns the matching node
 * or NULL.  Trailing slash is allowed.  Path "/" is the root. */
vfs_node_t *vfs_lookup(const char *path);

/* ---- current working directory + general path resolution ---- */

/* The current working directory (root until first vfs_chdir). */
vfs_node_t *vfs_cwd(void);
int         vfs_chdir(vfs_node_t *dir);   /* 0 ok, -1 if not a dir */

/* Resolve a path that may be absolute ("/a/b") or relative ("a/b"),
 * honouring "." and ".." and trailing slashes, against the current
 * working directory.  Empty/NULL path resolves to the cwd.  Returns the
 * node, or NULL if any component is missing or a non-final component is
 * not a directory. */
vfs_node_t *vfs_resolve(const char *path);

/* Resolve everything up to (but not including) the final path
 * component.  On success returns the parent directory node and copies
 * the final component name into `leaf`.  Used by mkdir/create/rm to
 * split "dir/dir/leaf" into (parent, "leaf").  Returns NULL if the
 * parent path is missing / not a directory, or the final component is
 * empty or "." / "..". */
vfs_node_t *vfs_resolve_parent(const char *path, char *leaf, int leafmax);

/* Build the absolute path string of `node` into `buf` (e.g. "/home/user").
 * Returns the length, or -1 if it does not fit in `max`. */
int vfs_path(vfs_node_t *node, char *buf, int max);

/* Remove a regular file (frees its data + node).  Returns 0 / -1. */
int vfs_unlink(vfs_node_t *file);

/* Remove an empty directory (not the root).  Returns 0, or -1 if the
 * node is not a dir / is the root / is non-empty. */
int vfs_rmdir(vfs_node_t *dir);

/* Look up an immediate child of `dir` by name.  Returns the node or
 * NULL (also NULL if `dir` is not a directory). */
vfs_node_t *vfs_child(vfs_node_t *dir, const char *name);

/* Depth-first walk: visit(depth, node, ctx) is called for every
 * reachable node, with `depth` counting the number of ancestors
 * (root = 0).  Useful for the File-Tree window. */
typedef void (*vfs_visit_fn)(int depth, vfs_node_t *node, void *ctx);
void vfs_walk(vfs_node_t *node, int depth, vfs_visit_fn visit, void *ctx);

/* Tally for the Memory window. */
unsigned long vfs_node_count(void);
unsigned long vfs_total_file_bytes(void);

#endif /* XINU_RPI4_VFS_H */
