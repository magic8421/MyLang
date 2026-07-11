#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define SNPRINTF_OK(n, size) ((n) >= 0 && (size_t)(n) < (size))

static inline int strscpy(char* dst, const char* src, size_t size) {
    size_t i;
    if (size == 0) return -1;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    if (src[i] != '\0') {
        return -1;
    }
    return (int)i;
}

#define CHECK_STRSCPY(ret, msg) \
    do { \
        if ((ret) < 0) { \
            fprintf(stderr, "error: string truncation: %s\n", (msg)); \
            exit(1); \
        } \
    } while (0)

#define CHECK_SNPRINTF(n, size, msg) \
    do { \
        if (!SNPRINTF_OK((n), (size))) { \
            fprintf(stderr, "error: snprintf truncation: %s\n", (msg)); \
            exit(1); \
        } \
    } while (0)

#endif
