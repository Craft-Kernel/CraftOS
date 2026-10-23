#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * kernel memory for things too big to keep in a fixed place, like decoded
 * pictures and web pages. call heap_init once at boot with the memory size
 * the bootloader gave us. kmalloc hands back memory lined up on a 16 byte
 * boundary, or null if it's run out; kzalloc does the same and fills it with
 * zeros. kfree gives a block back, and krealloc makes one bigger, moving it
 * if it has to.
 */
void heap_init(uint32_t mem_upper_kib);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
void *krealloc(void *p, size_t size);
size_t heap_used(void);
size_t heap_size(void);
