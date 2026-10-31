// Ruffle Wii U port (wasm32-wasip1): libc functions jxrlib references that WASI libc doesn't have.
// Only the encoder (strenc.c) uses tmpnam; Ruffle only decodes.
#include <stddef.h>

char *tmpnam(char *str) {
    (void)str;
    return NULL;
}
