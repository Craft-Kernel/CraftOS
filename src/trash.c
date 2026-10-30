#include <fs.h>
#include <string.h>
#include <trash.h>

/*
 * something in the bin keeps its node, and just moves into ~/.trash. if
 * there's already something in there with the same name, it goes in as
 * "name~2", "name~3" and so on. a little table remembers each thing's
 * original folder and name, matched up by node and id, so a node slot that
 * got reused isn't mixed up with something that used to be in the bin.
 */

#define RECORDS 64

static struct {
    int node;
    uint32_t id;
    char from[128];
    char name[FS_NAME_MAX];
} records[RECORDS];

static int bin_folder(void)
{
    int n = fs_resolve(FS_ROOT, TRASH_DIR);
    if (n < 0)
        n = fs_create(FS_ROOT, TRASH_DIR, true);
    return n;
}

static int find_record(int node)
{
    for (int i = 0; i < RECORDS; i++)
        if (records[i].node == node && records[i].id && fs_alive(node, records[i].id))
            return i;
    return -1;
}

/* true if node is the bin itself, or anything inside it. */
static bool in_bin(int node, int bin)
{
    for (int n = node;; n = fs_node(n)->parent) {
        if (n == bin)
            return true;
        if (n == FS_ROOT)
            return false;
    }
}

/*
 * moves a file or folder into the bin. you can't bin the root folder, the bin
 * itself, or anything read-only. it picks a name that's free in the bin,
 * moves the node there, and writes down where it came from so trash_restore
 * can put it back later.
 */
int trash_put(int node)
{
    int bin = bin_folder();
    if (bin < 0)
        return bin;
    if (node == FS_ROOT || in_bin(node, bin))
        return FS_ERR_READONLY;
    struct fs_node *f = fs_node(node);
    if (f->readonly || fs_node(f->parent)->readonly)
        return FS_ERR_READONLY;

    char from[128], name[FS_NAME_MAX];
    fs_path(f->parent, from, sizeof(from));
    strlcpy(name, f->name, sizeof(name));

    char dest[192];
    int err = FS_ERR_EXISTS;
    for (int i = 1; i < 100 && err == FS_ERR_EXISTS; i++) {
        if (i == 1)
            ksnprintf(dest, sizeof(dest), "%s/%s", TRASH_DIR, name);
        else
            ksnprintf(dest, sizeof(dest), "%s/%s~%d", TRASH_DIR, name, i);
        if (fs_resolve(FS_ROOT, dest) >= 0)
            continue;
        err = fs_move(node, FS_ROOT, dest);
    }
    if (err < 0)
        return err;

    /*
     * reuse the record for this node if there is one, or a free one, or else
     * the oldest one
     */
    int slot = find_record(node);
    for (int i = 0; i < RECORDS && slot < 0; i++)
        if (!records[i].id || !fs_alive(records[i].node, records[i].id))
            slot = i;
    if (slot < 0)
        slot = 0;
    records[slot].node = node;
    records[slot].id = fs_node(node)->id;
    strlcpy(records[slot].from, from, sizeof(records[slot].from));
    strlcpy(records[slot].name, name, sizeof(records[slot].name));
    return 0;
}

int trash_list(int *out, int max)
{
    int bin = bin_folder();
    return bin < 0 ? 0 : fs_list(bin, out, max);
}

const char *trash_origin(int node)
{
    int r = find_record(node);
    return r >= 0 ? records[r].from : "";
}

/*
 * puts something back where it came from, under its old name. the folder it
 * came from has to still be there.
 */
int trash_restore(int node)
{
    int r = find_record(node);
    if (r < 0)
        return FS_ERR_NOT_FOUND;
    char dest[192];
    ksnprintf(dest, sizeof(dest), "%s/%s", strcmp(records[r].from, "/") ? records[r].from : "", records[r].name);
    if (fs_resolve(FS_ROOT, dest) >= 0)
        return FS_ERR_EXISTS;
    int err = fs_move(node, FS_ROOT, dest);
    if (err < 0)
        return err;
    records[r].id = 0;
    return 0;
}

int trash_delete(int node)
{
    int r = find_record(node);
    int err = fs_remove_tree(node);
    if (err >= 0 && r >= 0)
        records[r].id = 0;
    return err < 0 ? err : 0;
}

int trash_empty(void)
{
    int items[FS_MAX_NODES];
    int count = trash_list(items, FS_MAX_NODES);
    for (int i = 0; i < count; i++)
        trash_delete(items[i]);
    return count;
}
