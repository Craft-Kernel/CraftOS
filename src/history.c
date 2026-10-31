#include <fs.h>
#include <heap.h>
#include <shell.h>
#include <string.h>

/*
 * the command history. every line the shell runs gets added to the end of
 * /home/crafter/cmd-history, one command per line, and the up and down keys
 * walk back through it. both shells (the text screen and the terminal
 * window) share the one file, and it's read fresh every time, so editing or
 * deleting it in notepad or with rm works just like you'd expect.
 */

#define HISTORY_FILE FS_HOME "/cmd-history"
#define HISTORY_MAX  500

/*
 * reads the whole file into memory from kmalloc. the caller frees it. hands
 * back null if there's no file or it's empty.
 */
static char *load(int *len)
{
    int n = fs_resolve(FS_ROOT, HISTORY_FILE);
    if (n < 0 || fs_node(n)->is_dir)
        return 0;
    int size = fs_size(n);
    if (size <= 0)
        return 0;
    char *text = kmalloc(size + 1);
    if (!text)
        return 0;
    *len = fs_read(n, text, size);
    text[*len] = '\0';
    return text;
}

/*
 * adds a line to the end of the file, making the file first if it isn't
 * there. blank lines and a repeat of the very last command get skipped, so
 * pressing up doesn't have to step through the same thing five times. once
 * the file holds more than HISTORY_MAX commands, the oldest ones get dropped.
 */
void shell_history_add(const char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;

    char last[SHELL_LINE_MAX + 1];
    if (shell_history_get(1, last, sizeof(last)) && !strcmp(last, line))
        return;

    int n = fs_resolve(FS_ROOT, HISTORY_FILE);
    if (n < 0)
        n = fs_create(FS_ROOT, HISTORY_FILE, false);
    if (n < 0 || fs_node(n)->is_dir)
        return;
    fs_write(n, line, strlen(line), true);
    fs_write(n, "\n", 1, true);

    int len;
    char *text = load(&len);
    if (!text)
        return;
    int lines = 0;
    for (int i = 0; i < len; i++)
        lines += text[i] == '\n';
    if (lines > HISTORY_MAX) {
        char *keep = text;
        for (int drop = lines - HISTORY_MAX; drop > 0; keep++)
            drop -= *keep == '\n';
        fs_write(n, keep, len - (keep - text), false);
    }
    kfree(text);
}

/*
 * copies out the command `back` steps from the end (1 is the newest). hands
 * back false when the history doesn't go back that far.
 */
bool shell_history_get(int back, char *buf, int size)
{
    int len;
    char *text = load(&len);
    if (!text)
        return false;

    /* walk backwards from the end, one line at a time, skipping blank ones */
    bool found = false;
    int end = len;
    while (end > 0 && !found) {
        while (end > 0 && text[end - 1] == '\n')
            end--;
        if (!end)
            break;
        int start = end;
        while (start > 0 && text[start - 1] != '\n')
            start--;
        if (--back == 0) {
            int n = end - start < size - 1 ? end - start : size - 1;
            memcpy(buf, text + start, n);
            buf[n] = '\0';
            found = true;
        }
        end = start;
    }
    kfree(text);
    return found;
}

/*
 * one press of up or down. leaving the line you were typing (back 0) tucks it
 * away in draft first, so coming all the way back down gives it back.
 */
bool shell_history_step(struct shell_recall *r, int dir, char *line, int *len)
{
    int back = r->back - dir;
    if (back < 0)
        return false;
    char next[SHELL_LINE_MAX + 1];
    if (back == 0)
        strlcpy(next, r->draft, sizeof(next));
    else if (!shell_history_get(back, next, sizeof(next)))
        return false;

    if (r->back == 0) {
        line[*len] = '\0';
        strlcpy(r->draft, line, sizeof(r->draft));
    }
    r->back = back;
    strlcpy(line, next, SHELL_LINE_MAX + 1);
    *len = strlen(line);
    return true;
}
