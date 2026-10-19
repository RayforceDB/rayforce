/* Declarations the wire headers need, for the freestanding wasm32 layout
 * check only (test/wasm/layout_check.c). */
#include <stddef.h>
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
