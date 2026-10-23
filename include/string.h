#pragma once
#include <stdarg.h>
#include <stddef.h>

/*
 * gcc sometimes calls memset, memcpy, memmove and memcmp all by itself, even
 * in code that's meant to stand alone, so they have to exist somewhere.
 */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

/* the same, but not caring about upper or lower case. */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

/*
 * copies at most size - 1 characters and always ends the string properly,
 * like bsd's strlcpy.
 */
size_t strlcpy(char *dst, const char *src, size_t size);

/* printf-style formatting into a buffer. format.c says what it understands. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
