#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * a small printf (well, vsnprintf) that the console, the desktop and the apps
 * all share.
 *
 * it knows %c, %s, %d, %u, %x, %p and %%, with an optional width,
 * zero-padding (like %08x) and left-justifying (like %-20s). it never deals
 * with 64-bit numbers, which means it never needs libgcc's division helpers
 * (which the kernel doesn't have).
 *
 * like the real one, it always ends the buffer properly, and returns how long
 * the whole output would have been, even if it didn't all fit.
 */

struct out {
    char *buf;
    size_t size, len;
};

static void emit(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void emit_uint(struct out *o, uint32_t v, unsigned base, int width, char pad)
{
    char tmp[32];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width && n < (int)sizeof(tmp))
        tmp[n++] = pad;
    while (n)
        emit(o, tmp[--n]);
}

/*
 * the formatter itself: walks the format string, copying ordinary characters
 * across and filling in each % from the arguments, with the padding and
 * alignment asked for.
 */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;

        bool left = false;
        char pad = ' ';
        int width = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = strlen(s);
            for (int i = len; !left && i < width; i++)
                emit(&o, ' ');
            while (*s)
                emit(&o, *s++);
            for (int i = len; left && i < width; i++)
                emit(&o, ' ');
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                emit(&o, '-');
                emit_uint(&o, -(uint32_t)v, 10, width ? width - 1 : 0, pad);
            } else {
                emit_uint(&o, v, 10, width, pad);
            }
            break;
        }
        case 'u':
            emit_uint(&o, va_arg(ap, uint32_t), 10, width, pad);
            break;
        case 'x':
            emit_uint(&o, va_arg(ap, uint32_t), 16, width, pad);
            break;
        case 'p':
            emit(&o, '0');
            emit(&o, 'x');
            emit_uint(&o, (uint32_t)va_arg(ap, void *), 16, 8, '0');
            break;
        case '%':
            emit(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            emit(&o, '%');
            emit(&o, *fmt);
        }
    }

    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
