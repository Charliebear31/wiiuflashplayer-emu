// Fast fills for the Wii U's CPU: a plain store loop first reads every cache line it is about to overwrite,
// so clearing a big picture or filling a bitmap ran at half the memory's write speed. `dcbz` claims a
// 32-byte line as zeros without reading it. Cached memory only (the heaps here are).
#pragma once
#include <stdint.h>
#include <string.h>

#define RW_LINE 32u

static inline void rw_dcbz(void* p) { __asm__ volatile("dcbz 0,%0" : : "r"(p) : "memory"); }

static inline void rw_zero_fast(void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    if (n < 4 * RW_LINE) { memset(p, 0, n); return; }
    size_t head = (RW_LINE - ((uintptr_t)p & (RW_LINE - 1))) & (RW_LINE - 1);
    if (head) { memset(p, 0, head); p += head; n -= head; }
    while (n >= RW_LINE) { rw_dcbz(p); p += RW_LINE; n -= RW_LINE; }
    if (n) memset(p, 0, n);
}

// `count` words of `value` at a 4-byte aligned address.
static inline void rw_fill_words_fast(uint32_t* d, uint32_t count, uint32_t value) {
    if (value == 0) { rw_zero_fast(d, (size_t)count * 4); return; }
    if (count < 4 * RW_LINE / 4) { for (uint32_t i = 0; i < count; i++) d[i] = value; return; }
    uint32_t head = ((RW_LINE - ((uintptr_t)d & (RW_LINE - 1))) & (RW_LINE - 1)) / 4;
    for (uint32_t i = 0; i < head; i++) *d++ = value;
    count -= head;
    while (count >= 8) {
        rw_dcbz(d);
        d[0] = value; d[1] = value; d[2] = value; d[3] = value;
        d[4] = value; d[5] = value; d[6] = value; d[7] = value;
        d += 8; count -= 8;
    }
    while (count--) *d++ = value;
}
