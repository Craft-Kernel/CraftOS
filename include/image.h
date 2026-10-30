#pragma once
#include <stdint.h>

/*
 * a decoded picture: width times height pixels, one row after another, each
 * one 0xaarrggbb. an alpha of 255 is solid and 0 is completely see-through.
 */
struct image {
    int width, height;
    uint32_t *pixels;
};

/*
 * decodes a cimg file (tools/img2cimg.py has the format) into memory from
 * kmalloc. it gives back null if the file's missing, isn't a cimg, or there's
 * no memory left.
 */
struct image *image_load(const char *path);
struct image *image_decode(const uint8_t *data, int size);

/* gives a decoded picture's memory back. */
void image_free(struct image *img);
