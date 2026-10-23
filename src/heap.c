#include <heap.h>
#include <io.h>
#include <string.h>

/*
 * the kernel heap: all the memory from the end of the kernel up to the top of
 * ram, cut into blocks that sit one after another. every block starts with a
 * small 16 byte header saying how big it is (header included) and whether
 * it's in use, so you can walk from the first block through all of them.
 *
 * kmalloc takes the first free block that's big enough, and if there's a lot
 * left over it splits that off into a new free block. kfree just marks a
 * block as free; free blocks next to each other get joined back up the next
 * time kmalloc walks past them. it's simple, and plenty fast for the few
 * hundred blocks the system has at any one time.
 */

struct block {
    uint32_t size;
    uint32_t used;
    uint32_t magic;
    uint32_t pad;
};

#define MAGIC     0xC4AF7B10u
#define HEADER    sizeof(struct block)
#define MIN_SPLIT (HEADER + 32)

extern char kernel_end[];

static struct block *first;
static uintptr_t limit;
static size_t in_use;

static struct block *after(struct block *b)
{
    struct block *n = (struct block *)((char *)b + b->size);
    return (uintptr_t)n < limit ? n : 0;
}

void heap_init(uint32_t mem_upper_kib)
{
    first = (struct block *)(((uintptr_t)kernel_end + 15) & ~(uintptr_t)15);
    limit = (0x100000 + (uintptr_t)mem_upper_kib * 1024) & ~(uintptr_t)15;
    first->size = limit - (uintptr_t)first;
    first->used = 0;
    first->magic = MAGIC;
    in_use = 0;
}

/*
 * finds room for size bytes: walks the blocks looking for the first free one
 * that's big enough, joining up free neighbours on the way, and splits off
 * whatever it doesn't need. returns null if there's no room anywhere.
 */
void *kmalloc(size_t size)
{
    if (size == 0)
        size = 1;
    size_t need = ((size + 15) & ~(size_t)15) + HEADER;

    uint32_t flags = irq_save();
    void *p = 0;
    for (struct block *b = first; b; b = after(b)) {
        if (b->used)
            continue;

        /* join up any free blocks right after this one */
        struct block *n;
        while ((n = after(b)) && !n->used)
            b->size += n->size;

        if (b->size < need)
            continue;
        if (b->size - need >= MIN_SPLIT) {
            struct block *rest = (struct block *)((char *)b + need);
            rest->size = b->size - need;
            rest->used = 0;
            rest->magic = MAGIC;
            b->size = need;
        }
        b->used = 1;
        in_use += b->size;
        p = b + 1;
        break;
    }
    irq_restore(flags);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/*
 * freeing null is fine, and so is anything that didn't come from kmalloc:
 * it's just left alone.
 */
void kfree(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    if (b->magic != MAGIC || !b->used)
        return;
    uint32_t flags = irq_save();
    b->used = 0;
    in_use -= b->size;
    irq_restore(flags);
}

/*
 * makes a block bigger, moving it somewhere else if it has to. like the usual
 * realloc, a null p just means kmalloc.
 */
void *krealloc(void *p, size_t size)
{
    if (!p)
        return kmalloc(size);
    struct block *b = (struct block *)p - 1;
    size_t have = b->size - HEADER;
    if (size <= have)
        return p;
    void *q = kmalloc(size);
    if (q) {
        memcpy(q, p, have);
        kfree(p);
    }
    return q;
}

size_t heap_used(void)
{
    return in_use;
}

size_t heap_size(void)
{
    return limit - (uintptr_t)first;
}
