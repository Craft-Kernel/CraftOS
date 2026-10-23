#pragma once
#include <stdint.h>

/* builds the craftos folder layout on the ram disk. call it after fs_init(). */
void sysfiles_init(uint32_t mem_upper_kib);
