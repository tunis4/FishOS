#pragma once

#include <klib/common.hpp>

// mem functions are implemented in mem.asm
extern "C" int memcmp(const void *lhs, const void *rhs, usize size);
extern "C" void* memcpy(void *dst, const void *src, usize size);
extern "C" void* memmove(void *dst, const void *src, usize size);
extern "C" void* memset(void *dst, u8 value, usize size);

#define memcmp __builtin_memcmp
#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memset __builtin_memset

namespace klib {
    usize strlen(const char *str);
    usize strnlen(const char *str, usize maxlen);
    char* strcpy(char *dst, const char *src);
    char* strncpy(char *dst, const char *src, usize n);
    char* strdup(const char *src);
    char* strcat(char *dst, const char *src);
    int strcmp(const char *lhs, const char *rhs);
    int strncmp(const char *lhs, const char *rhs, usize count);
    char* strchr(char *str, char c);
    const char* strchr(const char *str, char c);
    char* strstr(const char *str, const char *substr);
}
