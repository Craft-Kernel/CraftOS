#include <fs.h>
#include <heap.h>
#include <string.h>

static struct fs_node nodes[FS_MAX_NODES];
static uint32_t next_id = 1;

const char *fs_error(int err)
{
    switch (err) {
    case FS_ERR_NOT_FOUND:   return "no such file or folder";
    case FS_ERR_EXISTS:      return "already exists";
    case FS_ERR_READONLY:    return "read-only";
    case FS_ERR_NOT_EMPTY:   return "folder isn't empty";
    case FS_ERR_FULL:        return "the RAM disk is full";
    case FS_ERR_BAD_NAME:    return "bad name";
    case FS_ERR_NOT_DIR:     return "not a folder";
    case FS_ERR_IS_DIR:      return "is a folder";
    case FS_ERR_INTO_ITSELF: return "can't move a folder into itself";
    default:                 return "error";
    }
}

struct fs_node *fs_node(int n)
{
    return &nodes[n];
}

bool fs_alive(int n, uint32_t id)
{
    return n >= 0 && n < FS_MAX_NODES && nodes[n].used && nodes[n].id == id;
}

bool fs_editable(int n)
{
    struct fs_node *f = &nodes[n];
    return f->used && !f->is_dir && !f->readonly && !f->generate && !f->blob && !f->discard_writes;
}

static bool valid_name(const char *name)
{
    int len = strlen(name);
    if (len == 0 || len >= FS_NAME_MAX)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return !strchr(name, '/');
}

static int lookup(int dir, const char *name)
{
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == dir && strcmp(nodes[i].name, name) == 0)
            return i;
    return FS_ERR_NOT_FOUND;
}

/*
 * follows a path one folder at a time. empty bits (from "//" or a slash on
 * the end) and "." are skipped, and ".." at the very top just stays at the
 * top, like it does everywhere else.
 */
int fs_resolve(int cwd, const char *path)
{
    int n = cwd;
    if (path[0] == '/') {
        n = FS_ROOT;
    } else if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        n = fs_resolve(FS_ROOT, FS_HOME);
        path++;
        if (n < 0)
            return n;
    }

    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;

        char part[FS_NAME_MAX];
        int len = 0;
        while (*path && *path != '/') {
            if (len == FS_NAME_MAX - 1)
                return FS_ERR_NOT_FOUND;
            part[len++] = *path++;
        }
        part[len] = '\0';

        if (!nodes[n].is_dir)
            return FS_ERR_NOT_DIR;
        if (strcmp(part, "..") == 0)
            n = nodes[n].parent;
        else if (strcmp(part, ".") != 0 && (n = lookup(n, part)) < 0)
            return n;
    }
    return n;
}

/*
 * splits "a/b/name" into the folder "a/b" (which it finds and returns) and
 * "name" (which it copies out). slashes on the end don't count, so "a/b/"
 * splits into "a" and "b".
 */
static int split(int cwd, const char *path, char *name)
{
    char buf[256];
    strlcpy(buf, path, sizeof(buf));
    int len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';

    char *slash = strrchr(buf, '/');
    if (!slash) {
        strlcpy(name, buf, FS_NAME_MAX);
        return strlen(buf) < FS_NAME_MAX ? cwd : FS_ERR_BAD_NAME;
    }

    if (strlen(slash + 1) >= FS_NAME_MAX)
        return FS_ERR_BAD_NAME;
    strlcpy(name, slash + 1, FS_NAME_MAX);
    *slash = '\0';
    return fs_resolve(cwd, slash == buf ? "/" : buf);
}

static int new_node(int parent, const char *name, bool is_dir)
{
    for (int i = 1; i < FS_MAX_NODES; i++) {
        if (nodes[i].used)
            continue;
        memset(&nodes[i], 0, sizeof(nodes[i]) - FS_FILE_MAX);
        nodes[i].used = true;
        nodes[i].is_dir = is_dir;
        nodes[i].id = next_id++;
        nodes[i].parent = parent;
        strlcpy(nodes[i].name, name, FS_NAME_MAX);
        return i;
    }
    return FS_ERR_FULL;
}

int fs_create(int cwd, const char *path, bool is_dir)
{
    char name[FS_NAME_MAX];
    int parent = split(cwd, path, name);
    if (parent < 0)
        return parent;
    if (!nodes[parent].is_dir)
        return FS_ERR_NOT_DIR;
    if (!valid_name(name))
        return FS_ERR_BAD_NAME;
    if (lookup(parent, name) >= 0)
        return FS_ERR_EXISTS;
    if (nodes[parent].readonly)
        return FS_ERR_READONLY;
    return new_node(parent, name, is_dir);
}

int fs_remove(int n)
{
    if (n == FS_ROOT || nodes[n].readonly || nodes[nodes[n].parent].readonly)
        return FS_ERR_READONLY;
    if (nodes[n].is_dir)
        for (int i = 1; i < FS_MAX_NODES; i++)
            if (nodes[i].used && nodes[i].parent == n)
                return FS_ERR_NOT_EMPTY;
    kfree(nodes[n].big);
    nodes[n].big = 0;
    nodes[n].big_cap = 0;
    nodes[n].used = false;
    return 0;
}

/*
 * removes a folder and everything inside it, working from the deepest bits
 * up, since a folder has to be empty before it can go.
 */
int fs_remove_tree(int n)
{
    if (n == FS_ROOT || nodes[n].readonly || nodes[nodes[n].parent].readonly)
        return FS_ERR_READONLY;
    if (nodes[n].is_dir) {
        for (int i = 1; i < FS_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].parent == n && i != n) {
                int err = fs_remove_tree(i);
                if (err < 0)
                    return err;
            }
        }
    }
    return fs_remove(n);
}

/*
 * moves or renames something. if the destination is a folder, it goes inside
 * it with the same name; otherwise the destination's last part becomes the
 * new name. it won't overwrite anything, won't touch read-only places, and
 * won't let a folder be moved inside itself (which would cut it off from
 * everything).
 */
int fs_move(int n, int cwd, const char *dest)
{
    if (n == FS_ROOT || nodes[n].readonly || nodes[nodes[n].parent].readonly)
        return FS_ERR_READONLY;

    char name[FS_NAME_MAX];
    int parent;
    int target = fs_resolve(cwd, dest);
    if (target >= 0 && nodes[target].is_dir) {
        parent = target;
        strlcpy(name, nodes[n].name, sizeof(name));
    } else if (target >= 0) {
        return FS_ERR_EXISTS;
    } else if ((parent = split(cwd, dest, name)) < 0) {
        return parent;
    }

    if (!nodes[parent].is_dir)
        return FS_ERR_NOT_DIR;
    if (!valid_name(name))
        return FS_ERR_BAD_NAME;
    int clash = lookup(parent, name);
    if (clash >= 0 && clash != n)
        return FS_ERR_EXISTS;
    if (nodes[parent].readonly)
        return FS_ERR_READONLY;

    for (int p = parent;; p = nodes[p].parent) {
        if (p == n)
            return FS_ERR_INTO_ITSELF;
        if (p == FS_ROOT)
            break;
    }

    nodes[n].parent = parent;
    strlcpy(nodes[n].name, name, FS_NAME_MAX);
    return n;
}

static bool sorts_before(int a, int b)
{
    if (nodes[a].is_dir != nodes[b].is_dir)
        return nodes[a].is_dir;
    return strcmp(nodes[a].name, nodes[b].name) < 0;
}

int fs_list(int dir, int *out, int max)
{
    int count = 0;
    for (int i = 1; i < FS_MAX_NODES && count < max; i++) {
        if (!nodes[i].used || nodes[i].parent != dir)
            continue;

        /*
         * sorting as we go, a bit like sorting a hand of cards. folders never
         * hold more than a few dozen things, so that's plenty fast.
         */
        int j = count++;
        while (j > 0 && sorts_before(i, out[j - 1])) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return count;
}

/*
 * builds the full path backwards, by climbing from the file up through its
 * parents to the root.
 */
void fs_path(int n, char *buf, int size)
{
    if (n == FS_ROOT) {
        strlcpy(buf, "/", size);
        return;
    }

    int chain[FS_MAX_NODES], depth = 0;
    for (; n != FS_ROOT; n = nodes[n].parent)
        chain[depth++] = n;

    int len = 0;
    buf[0] = '\0';
    while (depth--)
        len += ksnprintf(buf + len, len < size ? size - len : 0, "/%s", nodes[chain[depth]].name);
}

void fs_short_path(int n, char *buf, int size)
{
    char full[256];
    fs_path(n, full, sizeof(full));
    int home_len = strlen(FS_HOME);
    if (strncmp(full, FS_HOME, home_len) == 0 && (full[home_len] == '\0' || full[home_len] == '/'))
        ksnprintf(buf, size, "~%s", full + home_len);
    else
        strlcpy(buf, full, size);
}

int fs_read(int n, char *buf, int size)
{
    struct fs_node *f = &nodes[n];
    if (f->is_dir)
        return FS_ERR_IS_DIR;
    if (f->generate) {
        int len = f->generate(buf, size);
        return len < size ? len : size - 1;
    }
    const char *src = f->blob ? f->blob : f->big ? f->big : f->data;
    int len = f->size < size - 1 ? f->size : size - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return len;
}

const void *fs_blob(int n, int *size)
{
    struct fs_node *f = &nodes[n];
    *size = f->size;
    if (f->is_dir || f->generate)
        return 0;
    return f->blob ? f->blob : f->big ? f->big : f->data;
}

/*
 * live files don't have a size until they're made, so we make one to find
 * out.
 */
int fs_size(int n)
{
    static char scratch[FS_FILE_MAX];
    if (nodes[n].generate)
        return fs_read(n, scratch, sizeof(scratch));
    return nodes[n].size;
}

/*
 * writes to a file, either replacing what's there or adding to the end.
 * read-only files, live files and built-in blobs say no, and writes to
 * /dev/null just vanish.
 */
int fs_write(int n, const char *data, int len, bool append)
{
    struct fs_node *f = &nodes[n];
    if (f->is_dir)
        return FS_ERR_IS_DIR;
    if (f->discard_writes)
        return 0;
    if (f->readonly || f->generate || f->blob)
        return FS_ERR_READONLY;

    int start = append ? f->size : 0;
    if (start + len > FS_BIG_MAX)
        len = FS_BIG_MAX - start;
    int need = start + len;

    /*
     * files start out with 4 kib of room right inside their node. past that,
     * the contents move into a bigger buffer from kmalloc, sized in 16 kib
     * steps, and if it outgrows that one too, everything moves to a bigger
     * one again and the old buffer is freed.
     */
    if (need > FS_FILE_MAX && need > f->big_cap) {
        int cap = (need + 16383) & ~16383;
        char *bigger = kmalloc(cap);
        if (!bigger)
            return FS_ERR_FULL;
        memcpy(bigger, f->big ? f->big : f->data, f->size);
        kfree(f->big);
        f->big = bigger;
        f->big_cap = cap;
    }
    memcpy((f->big ? f->big : f->data) + start, data, len);
    f->size = need;
    return 0;
}

/* ---- building the layout at boot ---- */

/* follows the path, making any folders along the way that aren't there yet. */
int fs_add_dir(const char *path, bool readonly)
{
    int n = FS_ROOT;
    char part[FS_NAME_MAX];

    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        int len = 0;
        while (*path && *path != '/' && len < FS_NAME_MAX - 1)
            part[len++] = *path++;
        part[len] = '\0';

        int next = lookup(n, part);
        if (next < 0 && (next = new_node(n, part, true)) < 0)
            return next;
        n = next;
    }
    nodes[n].readonly = readonly;
    return n;
}

static int add_file_node(const char *path)
{
    char name[FS_NAME_MAX];
    char dir[256];
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    strlcpy(name, slash + 1, sizeof(name));
    *slash = '\0';

    int parent = dir[0] ? fs_resolve(FS_ROOT, dir) : FS_ROOT;
    if (parent < 0)
        parent = fs_add_dir(dir, false);
    if (parent < 0)
        return parent;

    int n = lookup(parent, name);
    return n >= 0 ? n : new_node(parent, name, false);
}

int fs_add_file(const char *path, const char *text, bool readonly)
{
    int n = add_file_node(path);
    if (n < 0)
        return n;
    nodes[n].readonly = false;
    fs_write(n, text, strlen(text), false);
    nodes[n].readonly = readonly;
    return n;
}

int fs_add_live(const char *path, fs_generator_t generate)
{
    int n = add_file_node(path);
    if (n < 0)
        return n;
    nodes[n].generate = generate;
    nodes[n].readonly = true;
    return n;
}

int fs_add_blob(const char *path, const void *data, int size)
{
    int n = add_file_node(path);
    if (n < 0)
        return n;
    nodes[n].blob = data;
    nodes[n].size = size;
    nodes[n].readonly = true;
    return n;
}

void fs_init(void)
{
    nodes[FS_ROOT].used = true;
    nodes[FS_ROOT].is_dir = true;
    nodes[FS_ROOT].readonly = true;
    nodes[FS_ROOT].id = next_id++;
    nodes[FS_ROOT].parent = FS_ROOT;
}
