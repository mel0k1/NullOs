#ifndef STRING_H
#define STRING_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Memory operations
void* kmemset(void* dst, int val, size_t n);
void* kmemcpy(void* dst, const void* src, size_t n);
void* kmemmove(void* dst, const void* src, size_t n);
int  kmemcmp(const void* a, const void* b, size_t n);

// String operations
size_t kstrlen(const char* s);
int    kstrcmp(const char* a, const char* b);
int    kstrncmp(const char* a, const char* b, size_t n);
char*  kstrcpy(char* dst, const char* src);
char*  kstrncpy(char* dst, const char* src, size_t n);
char*  kstrcat(char* dst, const char* src);
char*  kstrchr(const char* s, int c);
char*  kstrrchr(const char* s, int c);
char*  kstrstr(const char* haystack, const char* needle);
char*  kstrtok(char* str, const char* delim);
int    katoi(const char* s);
long   katol(const char* s);
void   kitea(s64 value, char* buf, int base);
int    kisdigit(int c);
int    kisspace(int c);
int    kisalpha(int c);
int    kisalnum(int c);
int    ktoupper(int c);
int    ktolower(int c);
char*  kstrupr(char* s);
char*  kstrlwr(char* s);
int    ksnprintf(char* buf, size_t size, const char* fmt, ...);
int    kvsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list args);

// Formatted print to VGA
// Supports: %d %u %x %X %s %c %p %% and 64-bit: %ld %lu %lx %lld %llu %llx %zd
int vga_printf(const char* fmt, ...);

// Formatted print to an arbitrary character output function
// (used by vga_printf, ksnprintf, serial_printf)
typedef void (*char_output_fn)(char c, void* ctx);
int kprintf_format(char_output_fn out, void* ctx, const char* fmt, __builtin_va_list args);

#ifdef __cplusplus
}
#endif

#endif // STRING_H
