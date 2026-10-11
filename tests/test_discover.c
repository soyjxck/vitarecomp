/* test_discover.c — what discovery makes of stored pointers.
 *
 * A function nothing calls directly is known only by a word that holds its
 * address, and whether such a word is believed decides whether the function
 * exists in the output. The module here is made by hand: three functions and
 * the words that name them, in the three ways the rules tell apart.
 *
 *   0x10  a destructor that only stores its class's table: MOVW first, no
 *         prologue. Named by the first slot of a two-slot virtual table.
 *   0x20  one with a prologue, named by the second slot.
 *   0x30  a leaf with no prologue, named by one word among other data.
 *   0x40  an accessor between two more, eight bytes that are all printable
 *         ("ldr r0, [r0, #4]; bx lr" is "@hpG"), named the same way.
 *   0x64  the middle of a sentence, named the same way: not a function.
 *
 * In a module without relocations the table is read as a table by the two
 * words every virtual table starts with, and the lone word is weak evidence:
 * walked, a root only if its body is clean. Printable bytes are taken for
 * text unless they return within a few instructions, which text does not.
 * In a module with relocations neither reading is made, because there the
 * relocations say what is a pointer.
 */

#include "analyze.h"
#include "reloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

#define BASE 0x81000000u
#define SIZE 0x200u

static uint8_t g_image[SIZE];
static vm_module g_module;

static void put16(uint32_t at, uint16_t v) { g_image[at] = (uint8_t)v; g_image[at + 1] = (uint8_t)(v >> 8); }
static void put32(uint32_t at, uint32_t v) { put16(at, (uint16_t)v); put16(at + 2, (uint16_t)(v >> 16)); }

static void build(vm_image *img) {
    for (uint32_t at = 0; at < SIZE; at += 2) put16(at, 0xBF00);        /* nop */

    /* movw r1, #0x9418; movt r1, #0x818b; adds r1, #8; str r1, [r0]; bx lr */
    put16(0x10, 0xF249); put16(0x12, 0x4118); put16(0x14, 0xF2C8); put16(0x16, 0x118B);
    put16(0x18, 0x3108); put16(0x1A, 0x6001); put16(0x1C, 0x4770);
    /* push {r4, lr}; pop {r4, pc} */
    put16(0x20, 0xB510); put16(0x22, 0xBD10);
    /* movs r0, #0x81; adds r0, #0x90; bx lr */
    put16(0x30, 0x2081); put16(0x32, 0x3090); put16(0x34, 0x4770);
    /* ldr r0, [r0, #4]; bx lr, three times over */
    for (uint32_t at = 0x3C; at < 0x48; at += 4) { put16(at, 0x6840); put16(at + 2, 0x4770); }
    memcpy(g_image + 0x60, "The quick brown fox", 20);

    /* the virtual table: offset to top, type information, two functions */
    put32(0x100, 0);
    put32(0x104, BASE + 0x180);
    put32(0x108, BASE + 0x10 + 1);
    put32(0x10C, BASE + 0x20 + 1);
    put32(0x110, 0x00000002);
    /* and a structure with one callback in it */
    put32(0x120, 0x12345678);
    put32(0x124, 0x9ABCDEF0);
    put32(0x128, BASE + 0x30 + 1);
    put32(0x12C, 0x00000004);
    put32(0x130, BASE + 0x40 + 1);
    put32(0x134, 0x00000006);
    put32(0x138, BASE + 0x64 + 1);
    put32(0x13C, 0x00000008);

    memset(img, 0, sizeof(*img));
    img->data = g_image;
    img->size = SIZE;
    img->seg_file = 0;
    img->seg_vaddr = BASE;
    img->seg_len = SIZE;
}

/* Why discovery has a function at `addr`: its VF_R_* bits, 0 for none. */
static unsigned reasons(const vf_result *r, uint32_t addr) {
    for (uint32_t i = 0; i < r->count; i++)
        if (r->funcs[i].addr == addr) return r->funcs[i].reasons ? r->funcs[i].reasons : 0x8000u;
    return 0;
}

int main(void) {
    vm_image img;
    vf_result r;
    printf("discovery:\n");

    build(&img);
    assert(vf_discover(&img, &g_module, NULL, &r) == 0);
    assert(reasons(&r, BASE + 0x10) & VF_R_SHAPE);                      /* by the table's head */
    assert(reasons(&r, BASE + 0x20) & VF_R_SHAPE);
    assert(reasons(&r, BASE + 0x30) & VF_R_WEAK_PTR);                   /* walked */
    assert(!(reasons(&r, BASE + 0x30) & VF_R_ROOTS));                   /* and no root by that alone */
    assert(reasons(&r, BASE + 0x40) & VF_R_WEAK_PTR);                   /* printable, and it returns */
    assert(reasons(&r, BASE + 0x64) == 0);                              /* printable, and it does not */
    assert(r.seeds_weak == 2);
    vf_free(&r);
    printf("  a module without relocations   ok\n");

    /* the same bytes in a module whose relocations name its pointers, and name none of these */
    vr_entry none;
    vr_table relocs;
    memset(&none, 0, sizeof(none));
    memset(&relocs, 0, sizeof(relocs));
    none.code = 0xFF;
    relocs.v = &none;
    relocs.n = relocs.cap = 1;
    build(&img);
    assert(vf_discover(&img, &g_module, &relocs, &r) == 0);
    assert(reasons(&r, BASE + 0x10) == 0);                              /* two words are not a table */
    assert(reasons(&r, BASE + 0x20) & VF_R_SHAPE);                      /* a prologue still is a function */
    assert(reasons(&r, BASE + 0x30) == 0);
    assert(reasons(&r, BASE + 0x40) == 0);
    assert(reasons(&r, BASE + 0x64) == 0);
    assert(r.seeds_weak == 0);
    vf_free(&r);
    printf("  a module with relocations      ok\n");

    printf("all discovery tests passed\n");
    return 0;
}
