#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * the ram disk: a fixed table of 256 entries (nodes), each one a folder or a
 * file. a file keeps its first 4 kib right inside its node, and if it grows
 * past that, it moves into memory from kmalloc and can get up to 1 mib. node
 * 0 is the root folder. it all lives in memory, so every boot starts over
 * from the same starting layout.
 *
 * a node can be read-only, which means it can't be edited, renamed or
 * deleted, and a read-only folder won't take anything new. a file can also be
 * live: instead of keeping bytes, it has a function that writes its contents
 * fresh every time someone reads it (that's how /proc and /dev work). and a
 * file can be a blob: read-only bytes of any size that live somewhere else in
 * memory. all the pictures built into the kernel are blobs.
 *
 * nodes are passed around by their number in the table. numbers get reused
 * after something is deleted, so anything that holds on to a node for a while
 * also keeps its id, and checks it with fs_alive() before trusting the number
 * again.
 */

#define FS_MAX_NODES 256
#define FS_NAME_MAX  24
#define FS_FILE_MAX  4096
#define FS_BIG_MAX   (1024 * 1024)
#define FS_ROOT      0
#define FS_HOME      "/home/crafter"

typedef int (*fs_generator_t)(char *buf, int size);

struct fs_node {
    bool used;
    bool is_dir;
    bool readonly;
    bool discard_writes;
    uint32_t id;
    int parent;
    char name[FS_NAME_MAX];
    fs_generator_t generate;
    const char *blob;
    char *big;
    int big_cap;
    int size;
    char data[FS_FILE_MAX];
};

/*
 * errors are negative, so a function can hand back either a node or one of
 * these.
 */
enum {
    FS_ERR_NOT_FOUND = -1,
    FS_ERR_EXISTS = -2,
    FS_ERR_READONLY = -3,
    FS_ERR_NOT_EMPTY = -4,
    FS_ERR_FULL = -5,
    FS_ERR_BAD_NAME = -6,
    FS_ERR_NOT_DIR = -7,
    FS_ERR_IS_DIR = -8,
    FS_ERR_INTO_ITSELF = -9,
};

const char *fs_error(int err);

void fs_init(void);

struct fs_node *fs_node(int n);
bool fs_alive(int n, uint32_t id);

/*
 * whether notepad (or anything else) is allowed to change this file's bytes
 * directly.
 */
bool fs_editable(int n);

/*
 * paths can start from the root ("/etc/motd") or from cwd, and can use "."
 * and "..". a "~" at the start means your home folder.
 */
int fs_resolve(int cwd, const char *path);
int fs_create(int cwd, const char *path, bool is_dir);
int fs_remove(int n);

/* removes a folder with everything in it (or just a file). */
int fs_remove_tree(int n);

/*
 * works like mv: if dest is a folder that's already there, n moves into it
 * and keeps its name; otherwise n moves and takes dest as its new name.
 */
int fs_move(int n, int cwd, const char *dest);

/*
 * fills out[] with what's in a folder: folders first, then files, each lot in
 * name order.
 */
int fs_list(int dir, int *out, int max);

void fs_path(int n, char *buf, int size);

/* the short "~" version of a path, for prompts and title bars. */
void fs_short_path(int n, char *buf, int size);

int fs_read(int n, char *buf, int size);
int fs_size(int n);

/* a file's bytes, without copying them; null for folders and live files. */
const void *fs_blob(int n, int *size);
int fs_write(int n, const char *data, int len, bool append);

/*
 * for building the system's layout at boot. these skip the read-only rules
 * and make any missing folders along the way.
 */
int fs_add_dir(const char *path, bool readonly);
int fs_add_file(const char *path, const char *text, bool readonly);
int fs_add_live(const char *path, fs_generator_t generate);
int fs_add_blob(const char *path, const void *data, int size);
