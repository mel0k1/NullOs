#include "../include/tests.h"
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/buddy.h"
#include "../include/fs.h"
#include "../include/scheduler.h"
#include "../include/timer.h"
#include "../include/spinlock.h"

// ============================================================
// Kernel self-test suite
//
// Regression tests for the bugs fixed during the audit plus general
// sanity checks of core subsystems. Run with the `test` command.
// Every test must restore the state it changes.
// ============================================================

static u32 tests_passed = 0;
static u32 tests_failed = 0;

#define TEST_BEGIN(name) do { vga_print("  [TEST] " name ": "); } while (0)
#define TEST_OK()   do { vga_print_color("PASS\n", VGA_LIGHT_GREEN, VGA_BLACK); tests_passed++; } while (0)
#define TEST_FAIL() do { vga_print_color("FAIL\n", VGA_LIGHT_RED, VGA_BLACK);   tests_failed++; } while (0)

#define CHECK(cond) do { if (!(cond)) { TEST_FAIL(); return; } } while (0)
#define CHECK_END(cond) do { if (!(cond)) { TEST_FAIL(); return; } TEST_OK(); } while (0)

// ============================================================
// string / printf
// ============================================================

static void test_string(void) {
    char buf[16];

    TEST_BEGIN("snprintf truncation");
    int n = ksnprintf(buf, sizeof(buf), "%s-%u", "averylongstring", 123456);
    CHECK(kstrlen(buf) == 15);          // 16-1 chars written
    CHECK(n > 15);                      // returns would-be length
    CHECK_END(buf[15] == '\0');

    TEST_BEGIN("strncpy bounds");
    kmemset(buf, 'X', sizeof(buf));
    kstrncpy(buf, "abc", 8);
    // POSIX strncpy semantics (string.c pads the tail with NULs):
    // expect "abc" + five NUL bytes, overwriting the 'X' fill.
    CHECK(kmemcmp(buf, "abc\0\0\0\0\0", 8) == 0);
    CHECK_END(true);

    TEST_BEGIN("atoi negative");
    CHECK_END(katoi("-42") == -42 && katoi("+7") == 7 && katoi("0x10") == 0);
}

// ============================================================
// heap
// ============================================================

static void test_heap(void) {
    TEST_BEGIN("heap alloc/write/free cycle");
    size_t used_before = heap_get_used();
    u8* p = (u8*)kmalloc(300);
    CHECK(p != NULL);
    for (int i = 0; i < 300; i++) p[i] = (u8)(i * 7 + 1);
    for (int i = 0; i < 300; i++) {
        if (p[i] != (u8)(i * 7 + 1)) { kfree(p); TEST_FAIL(); return; }
    }
    kfree(p);
    CHECK_END(heap_get_used() == used_before);

    TEST_BEGIN("heap many small allocations");
    used_before = heap_get_used();
    void* ptrs[32];
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        ptrs[i] = kmalloc(24 + (size_t)(i % 5) * 16);
        if (!ptrs[i]) { ok = 0; break; }
        // unique fill pattern to catch overlapping allocations
        kmemset(ptrs[i], (u8)(0x40 + i), 24 + (size_t)(i % 5) * 16);
    }
    for (int i = 0; i < 32 && ok; i++) {
        if (*(u8*)ptrs[i] != (u8)(0x40 + i)) ok = 0;
    }
    for (int i = 0; i < 32; i++) if (ptrs[i]) kfree(ptrs[i]);
    if (!ok || heap_get_used() != used_before) { TEST_FAIL(); return; }
    CHECK_END(true);

    // SEMANTICS CHANGE (dlmalloc era): double free is honest UB now,
    // not "detected and ignored" — catchable only under -DDL_DEBUG via
    // dlmalloc's internal assertions, so it can no longer be a runnable
    // positive test here. The closest regressable property is that the
    // allocator survives an alloc/free cycle repeatedly.
    TEST_BEGIN("heap alloc/free cycle repeatable");
    p = (u8*)kmalloc(64);
    CHECK(p != NULL);
    kfree(p);
    size_t used_now = heap_get_used();
    p = (u8*)kmalloc(64);
    CHECK(p != NULL);
    kfree(p);
    CHECK_END(heap_get_used() == used_now);

    TEST_BEGIN("heap krealloc preserves data");
    p = (u8*)kmalloc(64);
    CHECK(p != NULL);
    for (int i = 0; i < 64; i++) p[i] = (u8)(i * 3 + 5);
    p = (u8*)krealloc(p, 200);
    CHECK(p != NULL);
    int krok = 1;
    for (int i = 0; i < 64; i++) {
        if (p[i] != (u8)(i * 3 + 5)) { krok = 0; break; }
    }
    kmemset(p, 0xAB, 200);   // the grown tail must be writable too
    kfree(p);
    CHECK_END(krok == 1);
}

// ============================================================
// buddy
// ============================================================

static void test_buddy(void) {
    TEST_BEGIN("buddy alloc/free restores accounting");

    u64 free_before = buddy_get_free_pages();

    void* a = buddy_alloc_pages(1);
    void* b = buddy_alloc_pages(2);
    void* c = buddy_alloc_pages(4);
    void* d = buddy_alloc_pages(1);
    if (!a || !b || !c || !d) {
        // restore whatever we got
        if (a) buddy_free_pages(a, 1);
        if (b) buddy_free_pages(b, 2);
        if (c) buddy_free_pages(c, 4);
        if (d) buddy_free_pages(d, 1);
        TEST_FAIL();
        return;
    }

    // distinct blocks
    if (a == b || a == c || a == d || b == c || b == d || c == d) {
        buddy_free_pages(a, 1); buddy_free_pages(b, 2);
        buddy_free_pages(c, 4); buddy_free_pages(d, 1);
        TEST_FAIL();
        return;
    }

    buddy_free_pages(a, 1);
    buddy_free_pages(b, 2);
    buddy_free_pages(c, 4);
    buddy_free_pages(d, 1);

    // Exact restoration catches merge-accounting drift (the old bug:
    // free_pages over-counted by 2^order per coalesce).
    u64 free_after = buddy_get_free_pages();
    CHECK_END(free_after == free_before && free_after <= buddy_get_total_pages());

    TEST_BEGIN("buddy large order allocation");
    void* big = buddy_alloc_pages(64);
    if (!big) { TEST_FAIL(); return; }
    buddy_free_pages(big, 64);
    CHECK_END(buddy_get_free_pages() == free_before);
}

// ============================================================
// PMM
// ============================================================

static void test_pmm(void) {
    TEST_BEGIN("pmm page uniqueness");
    phys_addr_t a = pmm_alloc_page();
    phys_addr_t b = pmm_alloc_page();
    if (a == 0 || b == 0 || a == b) {
        if (a) pmm_free_page(a);
        if (b) pmm_free_page(b);
        TEST_FAIL();
        return;
    }
    pmm_free_page(a);
    pmm_free_page(b);
    CHECK_END((a % PAGE_SIZE) == 0 && (b % PAGE_SIZE) == 0);
}

// ============================================================
// filesystem
// ============================================================

static void test_fs(void) {
    const char* path = "/_selftest.txt";

    TEST_BEGIN("fs write/read roundtrip");
    s32 fd = fs_open(path, FS_WRITE | FS_CREATE);
    if (fd < 0) { TEST_FAIL(); return; }
    const char* msg = "selftest payload 123";
    fs_write(fd, msg, kstrlen(msg));
    fs_close(fd);

    fd = fs_open(path, FS_READ);
    if (fd < 0) { TEST_FAIL(); return; }
    char buf[32];
    kmemset(buf, 0, sizeof(buf));
    s64 n = fs_read(fd, buf, sizeof(buf));
    fs_close(fd);
    if (n != (s64)kstrlen(msg) || kmemcmp(buf, msg, kstrlen(msg)) != 0) {
        TEST_FAIL();
        return;
    }
    CHECK_END(true);

    TEST_BEGIN("fs read past EOF returns 0 (regression)");
    fd = fs_open(path, FS_READ);
    if (fd < 0) { TEST_FAIL(); return; }
    fs_seek(fd, 1000000, 0);     // far beyond EOF
    n = fs_read(fd, buf, sizeof(buf));   // old code underflowed here
    fs_close(fd);
    CHECK_END(n == 0);

    TEST_BEGIN("fs long name rejected safely (regression)");
    char longname[80];
    kmemset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    s32 err = fs_create_file(longname);   // old code smashed the stack
    CHECK(err != FS_OK);
    err = fs_mkdir(longname);
    CHECK(err != FS_OK);
    TEST_OK();

    TEST_BEGIN("fs delete closes open fds (regression UAF)");
    fd = fs_open(path, FS_READ);
    if (fd < 0) { TEST_FAIL(); return; }
    s32 del = fs_delete_file(path);      // file is still open
    if (del != FS_OK) { fs_close(fd); TEST_FAIL(); return; }
    n = fs_read(fd, buf, sizeof(buf));   // fd must be invalid now
    CHECK(n == FS_ERR_INVAL);
    TEST_OK();
}

// ============================================================
// scheduler
// ============================================================

static void test_scheduler(void) {
    TEST_BEGIN("task_sleep_ms actually sleeps (regression)");
    u64 t0 = timer_get_ticks();
    task_sleep_ms(150);                  // 100Hz -> at least ~15 ticks
    u64 delta = timer_get_ticks() - t0;
    CHECK(delta >= 13);                  // small tolerance for tick jitter
    CHECK_END(delta < 400);              // ...but not absurdly long
}

// ============================================================
// slab
// ============================================================

static void test_slab(void) {
    TEST_BEGIN("slab ownership + double-free detection");
    slab_cache_t* cache = slab_cache_create("selftest", 64);
    CHECK(cache != NULL);

    void* o1 = slab_alloc(cache);
    void* o2 = slab_alloc(cache);
    void* o3 = slab_alloc(cache);
    if (!o1 || !o2 || !o3) { TEST_FAIL(); return; }
    CHECK(o1 != o2 && o2 != o3 && o1 != o3);

    size_t used = cache->used_objects;
    slab_free(cache, o2);            // normal free: used--
    CHECK(cache->used_objects == used - 1);
    slab_free(cache, o2);            // double free: rejected silently
    CHECK(cache->used_objects == used - 1);

    // foreign object must be rejected
    int foreign;
    slab_free(cache, &foreign);
    CHECK(cache->used_objects == used - 1);

    slab_free(cache, o1);
    slab_free(cache, o3);
    CHECK_END(cache->used_objects == 0);
}

// ============================================================
// spinlocks
// ============================================================

static void test_spinlock(void) {
    TEST_BEGIN("spinlock lock/unlock");
    static spinlock_t l;
    static int inited = 0;
    if (!inited) { l = SPINLOCK_INIT; inited = 1; }

    CHECK(!spin_is_locked(&l));
    u64 flags = spin_lock_irqsave(&l);
    CHECK(spin_is_locked(&l));
    spin_unlock_irqrestore(&l, flags);
    CHECK(!spin_is_locked(&l));

    // irqsave/irqrestore preserves and restores the IF flag. Locks are
    // NOT nestable (non-recursive xchg spinlock — nesting self-deadlocks
    // on the same CPU), so verify two SEQUENTIAL acquisitions instead.
    u64 f1 = spin_lock_irqsave(&l);
    CHECK(spin_is_locked(&l));
    spin_unlock_irqrestore(&l, f1);
    u64 f2 = spin_lock_irqsave(&l);
    CHECK(spin_is_locked(&l));
    spin_unlock_irqrestore(&l, f2);
    CHECK_END(!spin_is_locked(&l));
}

// ============================================================
// runner
// ============================================================

void cmd_test(int argc, char** argv) {
    (void)argc; (void)argv;

    vga_print("\nNullOs Kernel Self-Tests\n");
    vga_print("-------------------------\n");

    tests_passed = 0;
    tests_failed = 0;

    test_string();
    test_heap();
    test_buddy();
    test_pmm();
    test_fs();
    test_scheduler();
    test_slab();
    test_spinlock();

    vga_printf("\nResults: %u passed, %u failed\n",
               tests_passed, tests_failed);
    if (tests_failed == 0) {
        vga_print_color("ALL TESTS PASSED\n\n", VGA_LIGHT_GREEN, VGA_BLACK);
    } else {
        vga_print_color("SOME TESTS FAILED\n\n", VGA_LIGHT_RED, VGA_BLACK);
    }
}
