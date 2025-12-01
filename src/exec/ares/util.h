#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

#define PACKED __attribute__((__packed__))

#ifdef __wasm__
void *malloc(size_t size);
void free(void *ptr);
extern void panic();
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

#define ARES_CHECK_OOM(ptr) \
    if (!(ptr)) {             \
        panic();              \
    }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARES_CHECK_OOM(ptr)               \
    if (!(ptr)) {                           \
        fprintf(stderr, "out of memory\n"); \
        exit(137);                          \
    }
#endif

#define ARES_ARRAY_PUSH(arr)                                   \
    (((arr)->len) >= ((arr)->cap)                                \
     ? (arr)->buf = ares_array_grow((arr)->buf, &((arr)->cap), \
                                      sizeof(*((arr)->buf))),    \
     (arr)->buf + ((arr)->len)++ : (arr)->buf + ((arr)->len)++)

#define ARES_ARRAY_DEF(type) \
    typedef struct {           \
        size_t len;            \
        size_t cap;            \
        type *buf;             \
    }

#define ARES_ARRAY_TYPE(type) ARES_ARRAY_DEF(type) Array_##type

#define ARES_ARRAY(alias) Array_##alias
#define ARES_ARRAY_INIT(arr) (arr)->len = 0, (arr)->cap = 0
#define ARES_ARRAY_NEW(type) \
    (ARES_ARRAY(type)){.buf = NULL, .len = 0, .cap = 0}
#define ARES_ARRAY_FREE(arr) \
    free((arr)->buf), (arr)->buf = NULL, (arr)->len = (arr)->cap = 0
#define ARES_ARRAY_INSERT(arr, pos)                           \
    ((pos) >= ((arr)->cap)                                      \
     ? ares_array_grow((void **)&((arr)->buf), &((arr)->cap), \
                         sizeof(*((arr)->buf))),                \
     (arr)->buf + (pos) : (arr)->buf + (pos))
#define ARES_ARRAY_LEN(arr) (arr)->len
#define ARES_ARRAY_CAP(arr) (arr)->cap
#define ARES_ARRAY_GET(arr, pos) (&(arr)->buf[pos])
#define ARES_ARRAY_PREPARE(type, cap) \
    (ARES_ARRAY(type)) { (cap), (cap), NULL }
#define ARES_ARRAY_POP(arr) &((arr)->buf[--(arr)->len])
#define ARES_ARRAY_IS_EMPTY(arr) (!(arr)->buf || (arr)->len == 0)

#define ARES_CHECK_CALL(expr, fail_label) \
    if (!(expr)) {                          \
        goto fail_label;                    \
    }

static void *ares_array_grow(void *arr, size_t *cap, size_t size) {
    size_t oldcap = *cap;
    if (oldcap) *cap = oldcap * 2;
    else *cap = 4;
    void *newarr = malloc(*cap * size);
    ARES_CHECK_OOM(newarr);
    memset(newarr, 0, *cap * size);
    if (arr) {
        memcpy(newarr, arr, oldcap * size);
        free(arr);
    }
    return newarr;
}

static inline bool ares_buf_read(u8 *buf, int size, u32 *ret) {
    if (size == 1) {
        *ret = buf[0];
    } else if (size == 2) {
        *ret = buf[0];
        *ret |= ((u32)buf[1]) << 8;
    } else if (size == 4) {
        *ret = buf[0];
        *ret |= ((u32)buf[1]) << 8;
        *ret |= ((u32)buf[2]) << 16;
        *ret |= ((u32)buf[3]) << 24;
    } else {
        return false;
    }

    return true;
}

static inline bool ares_buf_write(u8 *buf, int size, u32 value) {
    if (size == 1) {
        buf[0] = value;
    } else if (size == 2) {
        buf[0] = value;
        buf[1] = value >> 8;
    } else if (size == 4) {
        buf[0] = value;
        buf[1] = value >> 8;
        buf[2] = value >> 16;
        buf[3] = value >> 24;
    } else {
        return false;
    }

    return true;
}
