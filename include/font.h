#pragma once
#include <stdint.h>

/*
 * a smooth (antialiased), proportional font, loaded from a .cfnt file. each
 * letter is a little grid of coverage values from 0 to 255: how much of each
 * pixel the letter covers. drawing mixes the text colour over whatever is
 * underneath by that much, so the edges come out smooth instead of jagged.
 *
 * it has all the printable ascii characters, from space to tilde.
 */

struct font_glyph {
    uint8_t advance;
    uint8_t width;
    int8_t left;
    uint8_t reserved;
    uint32_t offset;
} __attribute__((packed));

struct font {
    int first, count;
    int height, baseline;
    const struct font_glyph *glyphs;
    const uint8_t *coverage;
};

/*
 * reads a .cfnt from the ram disk, leaving the letter data where it is. null
 * if the file is missing or broken.
 */
struct font *font_load(const char *path);

const struct font_glyph *font_glyph(const struct font *f, char c);
