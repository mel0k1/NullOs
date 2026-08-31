#include "../include/string.h"
#include "../include/vga.h"

// ============================================================
// Memory operations
// ============================================================

void* kmemset(void* dst, int val, size_t n) {
    u8* d = (u8*)dst;
    while (n--) *d++ = (u8)val;
    return dst;
}

void* kmemcpy(void* dst, const void* src, size_t n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* kmemmove(void* dst, const void* src, size_t n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int kmemcmp(const void* a, const void* b, size_t n) {
    const u8* pa = (const u8*)a;
    const u8* pb = (const u8*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

// ============================================================
// String operations
// ============================================================

size_t kstrlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char* kstrcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* kstrncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* kstrcat(char* dst, const char* src) {
    kstrcpy(dst + kstrlen(dst), src);
    return dst;
}

char* kstrchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

char* kstrrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char*)last;
}

char* kstrstr(const char* haystack, const char* needle) {
    if (!needle[0]) return (char*)haystack;
    if (!haystack[0]) return NULL;

    for (size_t i = 0; haystack[i]; i++) {
        bool match = true;
        for (size_t j = 0; needle[j]; j++) {
            if (!haystack[i + j]) return NULL;
            if (haystack[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) return (char*)(haystack + i);
    }
    return NULL;
}

// Thread-local strtok state (single-threaded kernel is fine)
static char* strtok_next = NULL;

char* kstrtok(char* str, const char* delim) {
    if (str) {
        strtok_next = str;
    }
    if (!strtok_next) return NULL;

    // Skip leading delimiters
    char* start = strtok_next;
    while (*start && kstrchr(delim, *start)) {
        start++;
    }
    if (!*start) {
        strtok_next = NULL;
        return NULL;
    }

    // Find end of token
    char* end = start;
    while (*end && !kstrchr(delim, *end)) {
        end++;
    }
    if (*end) {
        *end = '\0';
        strtok_next = end + 1;
    } else {
        strtok_next = NULL;
    }
    return start;
}

int katoi(const char* s) {
    int result = 0;
    int sign = 1;

    while (kisspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (kisdigit(*s)) {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

long katol(const char* s) {
    long result = 0;
    long sign = 1;

    while (kisspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (kisdigit(*s)) {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

void kitea(s64 value, char* buf, int base) {
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (base < 2 || base > 36) { buf[0] = '\0'; return; }

    char tmp[65];
    int i = 0;
    int neg = 0;

    if (value < 0) {
        neg = 1;
        // Handle INT64_MIN safely: negate as unsigned
        tmp[i++] = digits[-(value % (s64)base)];
        value = -(value / (s64)base);
    }

    if (value == 0 && i == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }

    int pos = 0;
    if (neg) buf[pos++] = '-';
    while (i > 0) buf[pos++] = tmp[--i];
    buf[pos] = '\0';
}

int kisdigit(int c) { return c >= '0' && c <= '9'; }
int kisspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int kisalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int kisalnum(int c) { return kisdigit(c) || kisalpha(c); }
int ktoupper(int c) { return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c; }
int ktolower(int c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c; }

char* kstrupr(char* s) {
    for (char* p = s; *p; p++) *p = (char)ktoupper(*p);
    return s;
}

char* kstrlwr(char* s) {
    for (char* p = s; *p; p++) *p = (char)ktolower(*p);
    return s;
}

// ============================================================
// Core formatted output engine
// ============================================================

// Helper: output an unsigned number in any base (up to 36)
// Returns the number of characters output.
static int format_ubase(char_output_fn out, void* ctx, u64 value, int base, int uppercase) {
    char buf[65];
    const char* digits = uppercase ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   : "0123456789abcdef";
    int pos = 0;

    if (value == 0) {
        out('0', ctx);
        return 1;
    }

    while (value > 0) {
        buf[pos++] = digits[value % base];
        value /= base;
    }

    for (int i = pos - 1; i >= 0; i--) {
        out(buf[i], ctx);
    }
    return pos;
}

// Helper: output a signed number
// Returns the number of characters output.
static int format_signed(char_output_fn out, void* ctx, s64 value) {
    if (value < 0) {
        out('-', ctx);
        // Avoid UB: negate as unsigned
        return 1 + format_ubase(out, ctx, (u64)(0 - (u64)value), 10, 0);
    } else {
        return format_ubase(out, ctx, (u64)value, 10, 0);
    }
}

// Convert an unsigned value to a decimal/hex string (no NUL issues)
static void fmt_u64_str(u64 value, int base, int uppercase, char* buf) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[65];
    int pos = 0;
    if (value == 0) tmp[pos++] = '0';
    while (value > 0) { tmp[pos++] = digits[value % base]; value /= base; }
    int out = 0;
    while (pos > 0) buf[out++] = tmp[--pos];
    buf[out] = '\0';
}

// Emit a string honoring width / '-' (left align) / '0' (zero pad).
// A leading '-' is kept in front of zero padding.
static int emit_padded(char_output_fn out, void* ctx, const char* s,
                       int width, int zero, int left) {
    int n = 0;
    int len = 0;
    while (s[len]) len++;

    if (!left && zero && s[0] == '-') {
        out('-', ctx); n++; s++; len--;
    }
    int pad = (width > len) ? width - len : 0;
    if (!left) {
        char pc = zero ? '0' : ' ';
        for (int i = 0; i < pad; i++) { out(pc, ctx); n++; }
    }
    while (*s) { out(*s++, ctx); n++; }
    if (left) {
        for (int i = 0; i < pad; i++) { out(' ', ctx); n++; }
    }
    return n;
}

int kprintf_format(char_output_fn out, void* ctx, const char* fmt, __builtin_va_list args) {
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            out(*fmt, ctx);
            count++;
            fmt++;
            continue;
        }

        fmt++; // skip '%'

        // Flags: '-' (left align), '0' (zero pad)
        int left_align = 0;
        int zero_pad = 0;
        for (;;) {
            if (*fmt == '-')      { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1;  fmt++; }
            else break;
        }

        // Field width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Handle length modifiers
        int is_long = 0;
        int is_long_long = 0;

        if (*fmt == 'l') {
            fmt++;
            is_long = 1;
            if (*fmt == 'l') {
                fmt++;
                is_long_long = 1;
            }
        } else if (*fmt == 'z') {
            fmt++;
            is_long = sizeof(size_t) == 8 ? 1 : 0;
        }

        switch (*fmt) {
            case 'd': case 'i': {
                s64 val;
                if (is_long || is_long_long)
                    val = __builtin_va_arg(args, s64);
                else
                    val = (s64)__builtin_va_arg(args, s32);
                char num[24];
                fmt_u64_str((u64)(val < 0 ? 0 - (u64)val : (u64)val), 10, 0, num);
                if (val < 0) {
                    char negbuf[25];
                    negbuf[0] = '-';
                    for (int k = 0; k < 24; k++) { negbuf[k+1] = num[k]; if (!num[k]) break; }
                    count += emit_padded(out, ctx, negbuf, width, zero_pad, left_align);
                } else {
                    count += emit_padded(out, ctx, num, width, zero_pad, left_align);
                }
                break;
            }
            case 'u': {
                u64 val;
                if (is_long || is_long_long)
                    val = __builtin_va_arg(args, u64);
                else
                    val = (u64)__builtin_va_arg(args, u32);
                char num[24];
                fmt_u64_str(val, 10, 0, num);
                count += emit_padded(out, ctx, num, width, zero_pad, left_align);
                break;
            }
            case 'x': {
                u64 val;
                if (is_long || is_long_long)
                    val = __builtin_va_arg(args, u64);
                else
                    val = (u64)__builtin_va_arg(args, u32);
                char num[20];
                fmt_u64_str(val, 16, 0, num);
                count += emit_padded(out, ctx, num, width, zero_pad, left_align);
                break;
            }
            case 'X': {
                u64 val;
                if (is_long || is_long_long)
                    val = __builtin_va_arg(args, u64);
                else
                    val = (u64)__builtin_va_arg(args, u32);
                char num[20];
                fmt_u64_str(val, 16, 1, num);
                count += emit_padded(out, ctx, num, width, zero_pad, left_align);
                break;
            }
            case 'p': {
                u64 val = (u64)__builtin_va_arg(args, void*);
                out('0', ctx); out('x', ctx);
                count += 2 + format_ubase(out, ctx, val, 16, 0);
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(args, const char*);
                const char* null_str = "(null)";
                if (!s) s = null_str;
                count += emit_padded(out, ctx, s, width, 0, left_align);
                break;
            }
            case 'c': {
                char c = (char)__builtin_va_arg(args, int);
                out(c, ctx);
                count++;
                break;
            }
            case '%': {
                out('%', ctx);
                count++;
                break;
            }
            case '\0': {
                goto done;
            }
            default: {
                out('%', ctx);
                out(*fmt, ctx);
                count += 2;
                break;
            }
        }

        fmt++;
    }

done:
    return count;
}

// ============================================================
// VGA printf (convenience wrapper)
// ============================================================

static void vga_out_char(char c, void* ctx) {
    (void)ctx;
    vga_putchar(c);
}

int vga_printf(const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = kprintf_format(vga_out_char, NULL, fmt, args);
    __builtin_va_end(args);
    return ret;
}

// ============================================================
// snprintf (print to buffer)
// ============================================================

typedef struct {
    char* buf;
    size_t pos;
    size_t size;
} snprintf_ctx_t;

static void snprintf_out_char(char c, void* ctx) {
    snprintf_ctx_t* s = (snprintf_ctx_t*)ctx;
    if (s->pos + 1 < s->size) {
        s->buf[s->pos] = c;
    }
    s->pos++;
}

int kvsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list args) {
    if (!buf || size == 0) return 0;

    snprintf_ctx_t ctx = { .buf = buf, .pos = 0, .size = size };
    kprintf_format(snprintf_out_char, &ctx, fmt, args);

    // Null-terminate
    if (ctx.pos < size) {
        buf[ctx.pos] = '\0';
    } else {
        buf[size - 1] = '\0';
    }
    return (int)ctx.pos;
}

int ksnprintf(char* buf, size_t size, const char* fmt, ...) {
    if (!buf || size == 0) return 0;

    snprintf_ctx_t ctx = { .buf = buf, .pos = 0, .size = size };
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    kprintf_format(snprintf_out_char, &ctx, fmt, args);
    __builtin_va_end(args);
    // Null-terminate
    if (ctx.pos < size) {
        buf[ctx.pos] = '\0';
    } else {
        buf[size - 1] = '\0';
    }
    return (int)ctx.pos;
}
