#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void* _aligned_malloc(size_t size, size_t alignment);
void _aligned_free(void* block);
#ifdef __cplusplus
}
#endif
#define _mm_malloc(size, alignment) _aligned_malloc(size, alignment)
#define _mm_free(block) _aligned_free(block)
