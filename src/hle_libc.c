/* hle_libc.c — SceLibc, forwarded to the host C library.
 *
 * These are the easy ones, and they are easy for a specific reason: SceLibc is
 * the standard C library, so the semantics are already specified and already
 * implemented on the host. What is NOT free is the boundary — every pointer
 * argument is a guest address into a flat allocation, not a host pointer, and
 * every one has to be bounds-checked before it is touched. A forwarded
 * `memcpy` that skips that step is a guest-controlled write anywhere in the
 * host process.
 *
 * See hle.h for the calling convention and the link-order requirement.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_format.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- the guest/host boundary ------------------------------------------------ */

/* Translate a guest pointer, or trap. Returning NULL and continuing is not an
 * option here: the caller asked for a string to be copied, and a copy that did
 * not happen leaves the program running with the work undone — the same reason
 * a dispatch miss stops rather than returns. */
static void *gp(uint32_t addr, uint32_t len, const char *who) {
    if (len == 0) return NULL;              /* a zero-length op touches nothing */
    void *p = vita_mem_ptr(addr, len);
    if (!p) vita_trap_unimpl(addr, len, who);
    return p;
}

/* The length of a guest C string, bounded by what is actually backed.
 *
 * A host strlen on guest memory runs off the end of the allocation the moment
 * the guest passes something that is not a string, and it does so reading host
 * memory it has no business in. Scanning within the region turns that into a
 * named trap. */
static uint32_t gstrlen(uint32_t addr, const char *who) {
    uint32_t avail = vita_mem_avail(addr);
    if (avail == 0) { vita_trap_unimpl(addr, 0, who); return 0; }
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(addr, 1);
    if (!p) { vita_trap_unimpl(addr, 0, who); return 0; }
    const void *nul = memchr(p, 0, avail);
    if (!nul) { vita_trap_unimpl(addr, avail, who); return avail; }
    return (uint32_t)((const uint8_t *)nul - p);
}

/* --- C++ runtime and CRT init ------------------------------------------------
 *
 * The first firmware the module touches, before any of its own code runs. All
 * of these are bookkeeping for a dynamic-linking and multi-threading world
 * that a statically recompiled single module does not have, which is why they
 * are so short — they are correct as no-ops, not stubbed as ones.
 */

/* Records the DSO handle used to key static destructors. There is one module
 * and it never unloads, so there is nothing to key. */
void vita_hle___cxa_set_dso_handle_main(void) { }

/* Register a destructor to run at exit. Nothing here runs exit — the host
 * stops the program — so registration succeeds and the list is never walked.
 * Returning nonzero would make the C++ runtime treat static construction as
 * having failed. */
void vita_hle___cxa_atexit(void)   { r0 = 0; }
void vita_hle___aeabi_atexit(void) { r0 = 0; }

/* Guards for function-local static initialisation. The guard object's first
 * byte is "initialised"; acquire returns nonzero when the caller should run
 * the constructor.
 *
 * ponytail: single-threaded, so no contention state and no waiting. Two
 * recompiled threads reaching the same static would both construct it; revisit
 * when sceKernel threads land. */
void vita_hle___cxa_guard_acquire(void) {
    r0 = vita_read8(r0) ? 0u : 1u;
}
void vita_hle___cxa_guard_release(void) {
    vita_write8(r0, 1);
}

/* Thread-local storage registration for the module. No TLS, one thread. */
void vita_hle__sceLdTlsRegisterModuleInfo(void) { r0 = 0; }

/* --- errno -------------------------------------------------------------------
 *
 * `errno` is a macro for *__errno_loc(), so the guest needs an address it can
 * write through, which means the slot has to live in GUEST memory rather than
 * be a host int. It is carved out of the heap on first use for that reason.
 */

static uint32_t g_errno_slot;

void vita_hle__sceLibcErrnoLoc(void) { r0 = g_errno_slot; }

/* --- the guest heap ----------------------------------------------------------
 *
 * ponytail: a bump allocator. `free` is a no-op and `realloc` always copies to
 * fresh space, so a program that churns allocations will exhaust the region
 * even though its live set is small. That is the known ceiling, and it is the
 * right first move: C++ static initialisation allocates once and holds, which
 * is exactly the phase this needs to get through. Replace with a real free
 * list when a run dies on exhaustion rather than before.
 */

static uint32_t g_heap_base, g_heap_size, g_heap_used;

void vita_heap_init(uint32_t base, uint32_t size) {
    g_heap_base = base;
    g_heap_size = size;
    g_heap_used = 0;
    /* One word up front, so errno has somewhere to live before any malloc. */
    if (size >= 4) { g_errno_slot = base; g_heap_used = 4; }
}

uint32_t vita_heap_used(void) { return g_heap_used; }
uint32_t vita_heap_size(void) { return g_heap_size; }

static uint32_t heap_alloc(uint32_t n) {
    if (!g_heap_size) { vita_trap_unimpl(0, n, "malloc before vita_heap_init"); return 0; }

    /* Align the OFFSET, not just the size. Rounding sizes up keeps the cursor
     * aligned only while every reservation does the same, and the errno slot
     * does not — it is one word. Aligning here does not care who moved the
     * cursor or by how much. */
    uint32_t at_off = (g_heap_used + 7u) & ~7u;
    uint32_t size   = (n + 7u) & ~7u;
    if (size < n || at_off < g_heap_used) return 0;         /* either add wrapped */
    if (at_off > g_heap_size || size > g_heap_size - at_off) {
        vita_trap_unimpl(g_heap_used, n, "guest heap exhausted");
        return 0;
    }
    g_heap_used = at_off + size;
    return g_heap_base + at_off;
}

void vita_hle_malloc(void) {
    uint32_t n = r0;
    r0 = n ? heap_alloc(n) : 0u;
}

void vita_hle_free(void) { /* bump allocator: nothing to return */ }

/* void *memalign(size_t align, size_t size): the bump allocator rounds its
 * cursor up to the alignment, which costs the gap and nothing else. */
void vita_hle_memalign(void) {
    uint32_t align = r0, n = r1;
    if (align < 8 || (align & (align - 1))) align = 8;
    if (!g_heap_size) { vita_trap_unimpl(0, n, "memalign before vita_heap_init"); r0 = 0; return; }
    uint32_t at = (g_heap_base + g_heap_used + align - 1) & ~(align - 1);
    uint32_t size = (n + 7u) & ~7u;
    if (at - g_heap_base > g_heap_size || size > g_heap_size - (at - g_heap_base)) {
        vita_trap_unimpl(g_heap_used, n, "guest heap exhausted");
        r0 = 0; return;
    }
    g_heap_used = at - g_heap_base + size;
    r0 = n ? at : 0u;
}

/* void malloc_stats(void) */
void vita_hle_malloc_stats(void) {
    vita_hle_trace("malloc_stats: %u of %u bytes used", g_heap_used, g_heap_size);
}

void vita_hle_realloc(void) {
    uint32_t old = r0, n = r1;
    if (!n)   { r0 = 0; return; }
    if (!old) { r0 = heap_alloc(n); return; }
    /* The old size is unknown to a bump allocator, so copy what is certainly
     * readable and no more. Over-copying here would read past the original
     * allocation; under-copying loses data the caller still owns, so the bound
     * is what the region can back rather than a guess at the old size. */
    uint32_t at = heap_alloc(n);
    if (!at) { r0 = 0; return; }
    uint32_t avail = vita_mem_avail(old);
    uint32_t copy  = n < avail ? n : avail;
    void *d = gp(at, copy, "realloc dst");
    void *s = gp(old, copy, "realloc src");
    if (d && s) memmove(d, s, copy);
    r0 = at;
}

/* --- memory ------------------------------------------------------------------
 *
 * Sizes are known up front, so these are a bounds check and a host call.
 */

void vita_hle_memcpy(void) {
    void *d = gp(r0, r2, "memcpy dst"), *s = gp(r1, r2, "memcpy src");
    /* memmove, not memcpy: the guest may hand over overlapping regions, and
     * while ARM's memcpy is equally undefined there, the host's would corrupt
     * rather than trap and the bug would surface far away. */
    if (d && s) memmove(d, s, r2);
    /* memcpy returns its destination. */
}

void vita_hle_memmove(void) {
    void *d = gp(r0, r2, "memmove dst"), *s = gp(r1, r2, "memmove src");
    if (d && s) memmove(d, s, r2);
}

void vita_hle_memset(void) {
    void *d = gp(r0, r2, "memset");
    if (d) memset(d, (int)(r1 & 0xFF), r2);
}

void vita_hle_memcmp(void) {
    void *a = gp(r0, r2, "memcmp a"), *b = gp(r1, r2, "memcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)memcmp(a, b, r2) : 0u;
}

void vita_hle_memchr(void) {
    uint8_t *p = (uint8_t *)gp(r0, r2, "memchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), r2);
    /* A guest pointer back, not a host one: the offset is what carries over. */
    r0 = hit ? r0 + (uint32_t)(hit - p) : 0u;
}

/* --- strings -----------------------------------------------------------------
 *
 * Every one of these has to measure before it can act, because the length is
 * in the data. The measurement is the bounds check.
 */

void vita_hle_strlen(void) { r0 = gstrlen(r0, "strlen"); }

void vita_hle_strcmp(void) {
    uint32_t la = gstrlen(r0, "strcmp a"), lb = gstrlen(r1, "strcmp b");
    void *a = gp(r0, la + 1, "strcmp a"), *b = gp(r1, lb + 1, "strcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strcmp((char *)a, (char *)b) : 0u;
}

void vita_hle_strncmp(void) {
    uint32_t n = r2;
    uint32_t la = gstrlen(r0, "strncmp a"), lb = gstrlen(r1, "strncmp b");
    uint32_t ca = la + 1 < n ? la + 1 : n, cb = lb + 1 < n ? lb + 1 : n;
    void *a = gp(r0, ca, "strncmp a"), *b = gp(r1, cb, "strncmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strncmp((char *)a, (char *)b, n) : 0u;
}

void vita_hle_strcpy(void) {
    uint32_t n = gstrlen(r1, "strcpy src") + 1;
    void *d = gp(r0, n, "strcpy dst"), *s = gp(r1, n, "strcpy src");
    if (d && s) memcpy(d, s, n);
}

void vita_hle_strncpy(void) {
    uint32_t n = r2, l = gstrlen(r1, "strncpy src");
    void *d = gp(r0, n, "strncpy dst");
    if (!d) return;
    uint32_t copy = l < n ? l : n;
    void *s = gp(r1, copy, "strncpy src");
    if (s) memcpy(d, s, copy);
    /* strncpy pads the remainder with NULs — not just one, all of them. */
    if (copy < n) memset((uint8_t *)d + copy, 0, n - copy);
}

void vita_hle_strcat(void) {
    uint32_t ld = gstrlen(r0, "strcat dst"), ls = gstrlen(r1, "strcat src");
    void *d = gp(r0 + ld, ls + 1, "strcat dst"), *s = gp(r1, ls + 1, "strcat src");
    if (d && s) memcpy(d, s, ls + 1);
}

void vita_hle_strchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strchr");
    /* The terminator is findable: strchr(s, 0) returns the end of the string. */
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), l + 1);
    r0 = hit ? base + (uint32_t)(hit - p) : 0u;
}

void vita_hle_strrchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strrchr");
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strrchr");
    if (!p) { r0 = 0; return; }
    uint8_t c = (uint8_t)(r1 & 0xFF);
    for (uint32_t i = l + 1; i-- > 0; ) {
        if (p[i] == c) { r0 = base + i; return; }
    }
    r0 = 0;
}

void vita_hle_strstr(void) {
    uint32_t base = r0;
    uint32_t lh = gstrlen(base, "strstr haystack"), ln = gstrlen(r1, "strstr needle");
    char *h = (char *)gp(base, lh + 1, "strstr haystack");
    char *n = (char *)gp(r1, ln + 1, "strstr needle");
    if (!h || !n) { r0 = 0; return; }
    char *hit = strstr(h, n);
    r0 = hit ? base + (uint32_t)(hit - h) : 0u;
}

/* --- the printf family ---------------------------------------------------------
 *
 * One formatter (hle_format.h) and the two places its result can go: the
 * console, or guest memory with a bound. A FILE argument is answered as the
 * console whatever it points at: SceLibc's streams are the firmware's own
 * objects, which this runtime does not model, and a program printing to a
 * stream at this stage is reporting something worth seeing.
 */

static void print_console(uint32_t fmt, vita_vargs *v) {
    char out[2048];
    size_t n = vita_guest_vformat(out, sizeof(out), fmt, v);
    fputs(out, stdout);
    fflush(stdout);
    r0 = (uint32_t)n;
}

static void print_guest(uint32_t dst, uint32_t cap, uint32_t fmt, vita_vargs *v) {
    char out[4096];
    size_t n = vita_guest_vformat(out, sizeof(out), fmt, v);
    if (cap) {
        uint32_t take = (uint32_t)(n < cap - 1 ? n : cap - 1);
        if (take > sizeof(out) - 1) take = sizeof(out) - 1;
        void *d = gp(dst, take + 1, "sprintf dst");
        if (d) { memcpy(d, out, take); ((char *)d)[take] = '\0'; }
    }
    r0 = (uint32_t)n;
}

/* int printf(const char *fmt, ...) */
void vita_hle_printf(void)   { vita_vargs v = { 1, 0, 0 };  print_console(r0, &v); }
/* int vprintf(const char *fmt, va_list ap) */
void vita_hle_vprintf(void)  { vita_vargs v = { 0, 0, r1 }; print_console(r0, &v); }
/* int fprintf(FILE *f, const char *fmt, ...) */
void vita_hle_fprintf(void)  { vita_vargs v = { 2, 0, 0 };  print_console(r1, &v); }
/* int vfprintf(FILE *f, const char *fmt, va_list ap) */
void vita_hle_vfprintf(void) { vita_vargs v = { 0, 0, r2 }; print_console(r1, &v); }
/* int snprintf(char *dst, size_t n, const char *fmt, ...) */
void vita_hle_snprintf(void)  { vita_vargs v = { 3, 0, 0 };  print_guest(r0, r1, r2, &v); }
/* int vsnprintf(char *dst, size_t n, const char *fmt, va_list ap) */
void vita_hle_vsnprintf(void) { vita_vargs v = { 0, 0, r3 }; print_guest(r0, r1, r2, &v); }
/* int sprintf(char *dst, const char *fmt, ...): unbounded, so bounded by what
 * the destination has behind it. */
void vita_hle_sprintf(void)  { vita_vargs v = { 2, 0, 0 };  print_guest(r0, vita_mem_avail(r0), r1, &v); }
/* int vsprintf(char *dst, const char *fmt, va_list ap) */
void vita_hle_vsprintf(void) { vita_vargs v = { 0, 0, r2 }; print_guest(r0, vita_mem_avail(r0), r1, &v); }

/* int puts(const char *s) */
void vita_hle_puts(void) {
    uint32_t n = gstrlen(r0, "puts");
    const char *s = n ? (const char *)vita_mem_ptr(r0, n) : "";
    fprintf(stdout, "%.*s\n", (int)n, s);
    fflush(stdout);
    r0 = (uint32_t)n + 1;
}

/* --- leaving, loudly -------------------------------------------------------------- */

/* void exit(int status) */
void vita_hle_exit(void) {
    vita_hle_trace("exit(%d)", (int)r0);
    vita_host_exit((int)r0);
}

/* void abort(void) */
void vita_hle_abort(void) {
    fprintf(stderr, "vitarecomp: the program called abort()\n");
    vita_host_exit(134);
}

/* void _Assert(const char *message, const char *function): Sony's libc
 * reaches it from assert(); the message is "file:line expression". */
void vita_hle__Assert(void) {
    uint32_t n = vita_mem_avail(r0);
    const char *m = n ? (const char *)vita_mem_ptr(r0, 1) : NULL;
    uint32_t fn = vita_mem_avail(r1);
    const char *f = fn ? (const char *)vita_mem_ptr(r1, 1) : NULL;
    fprintf(stderr, "vitarecomp: assertion failed: %.*s%s%.*s\n",
            m ? (int)n : 6, m ? m : "(null)", f ? " in " : "", f ? (int)fn : 0, f ? f : "");
    vita_host_exit(134);
}

/* --- setjmp and longjmp ---------------------------------------------------------
 *
 * setjmp records the callee-saved state in the buffer and returns 0, which is
 * what every program that calls it expects on the way in. longjmp cannot be
 * answered: the saved state is a guest register file, but the place to resume
 * is a point inside a recompiled C function whose host frames are gone. A
 * program that gets there is told so by name, since silently returning from
 * longjmp is the one thing it cannot be. */

/* int setjmp(jmp_buf env): r4-r11, sp, lr, then d8-d15 */
void vita_hle_setjmp(void) {
    uint32_t env = r0;
    if (!vita_mem_ptr(env, 10 * 4 + 8 * 8)) { r0 = 0; return; }
    uint32_t regs[10] = { r4, r5, r6, r7, r8, r9, r10, r11, sp, lr };
    for (int i = 0; i < 10; i++) vita_write32(env + 4 * (uint32_t)i, regs[i]);
    for (int i = 0; i < 8; i++) {
        uint64_t d = vita_d_get(8 + i);
        vita_write32(env + 40 + 8 * (uint32_t)i, (uint32_t)d);
        vita_write32(env + 44 + 8 * (uint32_t)i, (uint32_t)(d >> 32));
    }
    r0 = 0;
}

/* void longjmp(jmp_buf env, int val) */
void vita_hle_longjmp(void) {
    vita_trap_unimpl(r0, r1, "longjmp: the host stack cannot be unwound to a guest setjmp");
    vita_host_exit(1);
}

/* --- rand -------------------------------------------------------------------------
 * The library's own generator, so a program seeded the same way draws the same
 * numbers: the Dinkumware linear congruence, 15 bits of result. */
static uint32_t g_randseed = 1;
void vita_hle_srand(void) { g_randseed = r0; }
void vita_hle_rand(void) {
    g_randseed = g_randseed * 1103515245u + 12345u;
    r0 = (g_randseed >> 16) & 0x7FFFu;
}

/* time_t time(time_t *t): seconds since 1970, 32 bits on this platform */
void vita_hle_time(void) {
    uint32_t now = (uint32_t)time(NULL);
    if (r0 && vita_mem_ptr(r0, 4)) vita_write32(r0, now);
    r0 = now;
}

/* --- the C++ runtime's type_info and exceptions ---------------------------------
 *
 * type_info is { vtable, const char *name } on this ABI, so name() is the
 * second word and equality is the names' equality. An exception thrown is the
 * end: the unwinder would need the tables the module ships and a way to land
 * in the middle of a recompiled function, and neither exists here. It says
 * which type was thrown, which is usually the diagnosis. */

/* const char *std::type_info::name() const */
void vita_hle__ZNKSt9type_info4nameEv(void) {
    r0 = vita_mem_ptr(r0, 8) ? vita_read32(r0 + 4) : 0;
}

/* bool std::type_info::operator==(const type_info &) const */
void vita_hle__ZNKSt9type_infoeqERKS_(void) {
    uint32_t a = vita_mem_ptr(r0, 8) ? vita_read32(r0 + 4) : 0;
    uint32_t b = vita_mem_ptr(r1, 8) ? vita_read32(r1 + 4) : 0;
    if (a == b) { r0 = 1; return; }
    uint32_t na = gstrlen(a, "type_info name"), nb = gstrlen(b, "type_info name");
    const char *sa = na ? (const char *)vita_mem_ptr(a, na) : NULL, *sb = nb ? (const char *)vita_mem_ptr(b, nb) : NULL;
    r0 = (sa && sb && na == nb && memcmp(sa, sb, na) == 0) ? 1u : 0u;
}

/* void *__cxa_allocate_exception(size_t size): the object, with room for the
 * header the unwinder would put before it */
void vita_hle___cxa_allocate_exception(void) {
    uint32_t n = r0 + 128;
    uint32_t at = heap_alloc(n);
    r0 = at ? at + 128 : 0;
}

/* void __cxa_throw(void *obj, std::type_info *tinfo, void (*dest)(void *)) */
void vita_hle___cxa_throw(void) {
    uint32_t name = vita_mem_ptr(r1, 8) ? vita_read32(r1 + 4) : 0;
    uint32_t n = name ? vita_mem_avail(name) : 0;
    const char *s = n ? (const char *)vita_mem_ptr(name, 1) : NULL;
    fprintf(stderr, "vitarecomp: C++ exception thrown, type %.*s: not unwound\n", s ? (int)n : 1, s ? s : "?");
    vita_trap_unimpl(r0, r1, "__cxa_throw: exceptions are not unwound");
    vita_host_exit(1);
}

/* std::bad_typeid::bad_typeid(): the object is the caller's; nothing to set
 * that a throw would read before the trap above ends things. */
void vita_hle__ZNSt10bad_typeidC1Ev(void) { }
void vita_hle__ZNSt9bad_allocC1Ev(void)   { }
void vita_hle__ZNSt8bad_castC1Ev(void)    { }
