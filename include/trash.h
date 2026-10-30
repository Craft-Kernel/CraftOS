#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <fs.h>

/*
 * the recycle bin. deleting something from the file manager (or with `trash`
 * in the shell) doesn't get rid of it straight away. it moves it into
 * ~/.trash and remembers where it came from, so you can put it back later.
 * emptying the bin, or deleting something from inside it, gets rid of it for
 * good.
 *
 * like everything else on the ram disk, the bin starts empty every time the
 * machine boots.
 */

#define TRASH_DIR FS_HOME "/.trash"

/* 0, or a file system error. */
int trash_put(int node);

/*
 * what's in the bin, as folder and file nodes in list order. returns how many
 * there are.
 */
int trash_list(int *out, int max);

/*
 * the folder something in the bin came from, like "/home/crafter/projects",
 * or "" if we don't know.
 */
const char *trash_origin(int node);

int trash_restore(int node);
int trash_delete(int node);

/* gets rid of everything, and returns how many things there were. */
int trash_empty(void);
