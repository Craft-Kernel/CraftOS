#pragma once

/*
 * programs that run in the background as threads of their own. each one has a
 * file in /bin, so typing its name in the shell starts it, and it shows up in
 * ps and the task manager until somebody kills it.
 */
struct program {
    const char *name;
    const char *usage;
    void (*main)(void);
};

extern const struct program programs[];
extern const int program_count;

const struct program *program_find(const char *name);
