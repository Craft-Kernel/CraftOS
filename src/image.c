#include <fs.h>
#include <heap.h>
#include <image.h>
#include <string.h>

/*
 * the cimg decoder. a cimg is a 10-byte header (the letters "cimg", the
 * width, the height and the encoding) followed by the pixels, either as runs
 * of (count, argb) or as raw b, g, r bytes. the file gets checked as it's
 * read, so a damaged one just fails instead of running off the end of its
 * data.
 */

#define CIMG_RLE 0
#define CIMG_RGB 1

static uint16_t read16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * decodes a cimg: checks the header, then reads either the raw rgb pixels
 * (all solid) or the runs of (count, colour), refusing anything that would go
 * past the end of the file or doesn't fill the picture.
 */
struct image *image_decode(const uint8_t *data, int size)
{
    if (size < 10 || memcmp(data, "CIMG", 4) != 0)
        return 0;
    int width = read16(data + 4), height = read16(data + 6), encoding = data[8];
    if (!width || !height)
        return 0;

    struct image *img = kmalloc(sizeof(*img));
    uint32_t *px = kmalloc((size_t)width * height * 4);
    if (!img || !px)
        return 0;

    int total = width * height, n = 0;
    const uint8_t *p = data + 10, *end = data + size;

    if (encoding == CIMG_RGB) {
        if (end - p < total * 3)
            return 0;
        for (; n < total; n++, p += 3)
            px[n] = 0xFF000000 | p[2] << 16 | p[1] << 8 | p[0];
    } else if (encoding == CIMG_RLE) {
        while (n < total && end - p >= 5) {
            int count = p[0];
            uint32_t color = read32(p + 1);
            p += 5;
            for (; count && n < total; count--)
                px[n++] = color;
        }
        if (n < total)
            return 0;
    } else {
        return 0;
    }

    img->width = width;
    img->height = height;
    img->pixels = px;
    return img;
}

struct image *image_load(const char *path)
{
    int n = fs_resolve(FS_ROOT, path);
    if (n < 0)
        return 0;
    int size;
    const uint8_t *data = fs_blob(n, &size);
    return data ? image_decode(data, size) : 0;
}

void image_free(struct image *img)
{
    if (!img)
        return;
    kfree(img->pixels);
    kfree(img);
}
