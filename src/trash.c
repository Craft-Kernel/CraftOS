#include <fs.h>
#include <string.h>
#include <trash.h>

/*
 * something in the bin keeps its node, and just moves into ~/.trash. if
 * there's already something in there with the same name, it goes in as
 * "name~2", "name~3" and so on. a little table remembers each thing's
 * original folder and name, matched up by node and id, so a node slot that
 * got reused isn't mixed up with something that used to be in the bin.
 *
 * AI Contribution (Bug Fix): the original folder is kept as its node and id,
 * not a written-out path. the path went into a 128 byte buffer, and a deep
 * folder's path got cut short, so restoring went to the wrong place or
 * failed. the path is only worked out now when something asks for it.
 */

#define RECORDS 64

static struct {
    int node;
    uint32_t id;
    int from;
    uint32_t from_id;
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

    int from = f->parent;
    uint32_t from_id = fs_node(from)->id;
    char name[FS_NAME_MAX], base[FS_NAME_MAX - 3];
    strlcpy(name, f->name, sizeof(name));
    /*
     * AI Contribution (Bug Fix): leave room for the "~NN" a duplicate gets.
     * a name already near the 24 character limit had no room for it, so the
     * move came back "bad name" and the file couldn't be binned at all.
     */
    strlcpy(base, name, sizeof(base));

    char dest[FS_NAME_MAX];
    int err = FS_ERR_EXISTS;
    for (int i = 1; i < 100 && err == FS_ERR_EXISTS; i++) {
        if (i == 1)
            strlcpy(dest, name, sizeof(dest));
        else
            ksnprintf(dest, sizeof(dest), "%s~%d", base, i);
        if (fs_resolve(bin, dest) >= 0)
            continue;
        err = fs_move(node, bin, dest);
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
    records[slot].from = from;
    records[slot].from_id = from_id;
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
    static char path[256];
    int r = find_record(node);
    if (r < 0 || !fs_alive(records[r].from, records[r].from_id))
        return "";
    fs_path(records[r].from, path, sizeof(path));
    return path;
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
    if (!fs_alive(records[r].from, records[r].from_id))
        return FS_ERR_NOT_FOUND;
    if (fs_resolve(records[r].from, records[r].name) >= 0)
        return FS_ERR_EXISTS;
    int err = fs_move(node, records[r].from, records[r].name);
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
