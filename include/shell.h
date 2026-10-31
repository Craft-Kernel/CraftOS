#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * the craftos shell. the same commands work in two places: right on the text
 * screen after boot, and inside the desktop's terminal window. each place has
 * its own struct shell, which says where the output goes and keeps its own
 * current folder.
 */

#define SHELL_LINE_MAX 120

struct shell {
    void (*write)(const char *s, int len);
    void (*clear)(void);
    bool in_desktop;

    /*
     * set while a background job (like ping in the desktop) is still printing
     */
    volatile bool busy;

    int cwd;
    uint32_t cwd_id;
};

/*
 * puts a file in /bin for every command and program. call it once at boot,
 * after the file system is ready.
 */
void shell_install(void);

/* goes to the home folder and prints the welcome message from /etc/motd. */
void shell_start(struct shell *sh);

void shell_prompt(struct shell *sh);
void shell_run(struct shell *sh, char *line);

/*
 * the command history in /home/crafter/cmd-history. shell_run adds every line
 * to it, and shell_history_get(1, ...) is the newest command, 2 the one
 * before that, and so on (false once it runs out).
 */
void shell_history_add(const char *line);
bool shell_history_get(int back, char *buf, int size);

/*
 * for the up and down keys while typing a line. each place that reads lines
 * keeps one of these, zeroed again after every enter. shell_history_step
 * swaps line for the next older (dir -1, up) or newer (dir 1, down) command,
 * and going down past the newest brings back whatever you'd been typing.
 * hands back false if there was nowhere to go, so the line didn't change.
 */
struct shell_recall {
    int back;
    char draft[SHELL_LINE_MAX + 1];
};
bool shell_history_step(struct shell_recall *r, int dir, char *line, int *len);
