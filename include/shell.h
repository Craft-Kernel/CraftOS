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
