#include <font.h>
#include <fs.h>
#include <heap.h>
#include <string.h>

/*
 * loads a cfnt font. the file starts with a 12-byte header ("cfnt", the
 * version, the first character, how many letters, the height and the
 * baseline), then 8 bytes for each letter (how far to move along, its width,
 * how far it starts to the left, and where its coverage grid is), then all
 * the coverage grids. nothing gets copied: the font just points into the
 * file, which is built into the kernel anyway. every letter is checked to
 * make sure it really fits inside the file before the font is handed out.
 */

struct font *font_load(const char *path)
{
    int n = fs_resolve(FS_ROOT, path);
    if (n < 0)
        return 0;
    int size;
    const uint8_t *data = fs_blob(n, &size);
    if (!data || size < 12 || memcmp(data, "CFNT", 4) != 0 || data[4] != 1)
        return 0;

    int count = data[6], height = data[7];
    int table = 12, cover = table + count * 8;
    if (count == 0 || height == 0 || cover > size)
        return 0;

    const struct font_glyph *glyphs = (const struct font_glyph *)(data + table);
    for (int i = 0; i < count; i++)
        if (cover + glyphs[i].offset + (uint32_t)glyphs[i].width * height > (uint32_t)size)
            return 0;

    struct font *f = kmalloc(sizeof(*f));
    if (!f)
        return 0;
    f->first = data[5];
    f->count = count;
    f->height = height;
    f->baseline = data[8];
    f->glyphs = glyphs;
    f->coverage = data + cover;
    return f;
}

/* anything the font doesn't have comes out as a question mark. */
const struct font_glyph *font_glyph(const struct font *f, char c)
{
    int i = (uint8_t)c - f->first;
    if (i < 0 || i >= f->count)
        i = '?' - f->first;
    return &f->glyphs[i];
}
