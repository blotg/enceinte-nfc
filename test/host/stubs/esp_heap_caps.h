#pragma once
#include <stdlib.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_SPIRAM 8
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_realloc(p, size, caps) realloc(p, size)
#define heap_caps_get_total_size(caps) ((size_t)((caps) == MALLOC_CAP_SPIRAM ? 0 : 1 << 20))
#define heap_caps_get_free_size(caps) ((size_t)200000)
