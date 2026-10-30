#pragma once
#include <gui.h>

/*
 * the apps that live in files of their own, and the ways they open each
 * other.
 */

extern struct window notepad_window;
extern struct window terminal_window;
extern struct window files_window;
extern struct window taskman_window;
extern struct window dialog_window;
extern struct window imgview_window;
extern struct window mines_window;
extern struct window solitaire_window;
extern struct window recycle_window;

void terminal_init(void);

/*
 * opens a file from the ram disk in notepad, or a folder in the file manager.
 */
void notepad_open_file(int node);
void files_open_at(int dir);
void imgview_open(int node);

/* whether a file name looks like a picture (a .cimg). */
bool is_image_name(const char *name);

/*
 * ending a task from the desktop. ordinary tasks end straight away, but for a
 * kernel task this pops up the warning dialog instead and returns
 * taskman_asked, and the task only dies if the user says yes.
 */
#define TASKMAN_ASKED 1
int taskman_kill(int pid);

#define KERNEL_KILL_WARNING \
    "This is a kernel task. Killing this task can and will crash your system, " \
    "possibly rendering it unusable depending on what's happening. Are you sure?"
