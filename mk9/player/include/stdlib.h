#pragma once
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#ifdef __cplusplus
extern "C" {
#endif
[[noreturn]] void abort(void);
void* malloc(size_t size);
void free(void* block);
void* calloc(size_t count, size_t size);
void* realloc(void* block, size_t size);
int abs(int);
long labs(long);
long long llabs(long long);
#ifdef __cplusplus
}
#endif
