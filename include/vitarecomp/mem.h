/* mem.h — the recompiled program's address space.
 *
 * The Vita has an MMU, so unlike the PSP there is no small fixed map to fold
 * onto a backing store with a mask. What there is instead is simpler in
 * practice: a module is loaded at a known base (0x81000000 for the executable
 * segment) and the interesting addresses are all within a bounded window above
 * it. A flat host allocation plus a subtraction covers it.
 *
 * Accesses outside the window are COUNTED rather than silently returning zero.
 * A recompiled program should reach a steady state with that counter flat;
 * when it climbs, discovery or translation missed something.
 */

#ifndef VITARECOMP_MEM_H
#define VITARECOMP_MEM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern uint32_t vita_mem_bad_access;

/* Reserve `size` bytes of guest address space starting at `base`. */
int  vita_mem_init(uint32_t base, size_t size);
void vita_mem_free(void);

/* Returns NULL rather than a wild pointer when an access would straddle the
 * end of the region, so a bad address is a diagnosable NULL dereference at the
 * point of failure instead of silent corruption elsewhere. */
void *vita_mem_ptr(uint32_t addr, uint32_t len);

/* Bytes backed from `addr` to the end of the region, or 0 if it is outside.
 *
 * The HLE needs this and `vita_mem_ptr` cannot answer it: a C string has no
 * length until you find its terminator, so forwarding `strlen` to the host
 * means scanning guest memory, and a scan with no bound runs off the
 * allocation when the guest hands over a pointer to something that is not a
 * string. Unlike a bad read this does not count as a bad access — asking how
 * much is there is a legitimate question. */
uint32_t vita_mem_avail(uint32_t addr);

/* The reads and writes of the program itself. A recompiled title makes a
 * hundred million of them a second, so the one that lands in the region,
 * which is every one of a program that is right, is done in line: a
 * subtraction, a comparison, the bytes. Anything else goes to the function
 * of the same name with _slow on it, which counts it as a bad access; and
 * every write goes there while a write watchpoint is set (the room for
 * writes is then nothing).
 *
 * Unaligned access is legal on ARMv7 for ordinary loads and stores, so the
 * bytes go through memcpy rather than a cast. */
extern uint8_t *vita_mem_host;              /* the host's bytes for the region's first address */
extern uint32_t vita_mem_first;             /* that address */
extern uint64_t vita_mem_read_room;         /* bytes from it that may be read, */
extern uint64_t vita_mem_write_room;        /* and that may be written in line */

uint32_t vita_read32_slow(uint32_t addr);
uint16_t vita_read16_slow(uint32_t addr);
uint8_t  vita_read8_slow (uint32_t addr);
void vita_write32_slow(uint32_t addr, uint32_t v);
void vita_write16_slow(uint32_t addr, uint16_t v);
void vita_write8_slow (uint32_t addr, uint8_t  v);

static inline uint32_t vita_read32(uint32_t addr) {
    uint32_t at = addr - vita_mem_first, v;
    if ((uint64_t)at + 4 > vita_mem_read_room) return vita_read32_slow(addr);
    memcpy(&v, vita_mem_host + at, 4);
    return v;
}
static inline uint16_t vita_read16(uint32_t addr) {
    uint32_t at = addr - vita_mem_first;
    uint16_t v;
    if ((uint64_t)at + 2 > vita_mem_read_room) return vita_read16_slow(addr);
    memcpy(&v, vita_mem_host + at, 2);
    return v;
}
static inline uint8_t vita_read8(uint32_t addr) {
    uint32_t at = addr - vita_mem_first;
    if ((uint64_t)at + 1 > vita_mem_read_room) return vita_read8_slow(addr);
    return vita_mem_host[at];
}
static inline void vita_write32(uint32_t addr, uint32_t v) {
    uint32_t at = addr - vita_mem_first;
    if ((uint64_t)at + 4 > vita_mem_write_room) { vita_write32_slow(addr, v); return; }
    memcpy(vita_mem_host + at, &v, 4);
}
static inline void vita_write16(uint32_t addr, uint16_t v) {
    uint32_t at = addr - vita_mem_first;
    if ((uint64_t)at + 2 > vita_mem_write_room) { vita_write16_slow(addr, v); return; }
    memcpy(vita_mem_host + at, &v, 2);
}
static inline void vita_write8(uint32_t addr, uint8_t v) {
    uint32_t at = addr - vita_mem_first;
    if ((uint64_t)at + 1 > vita_mem_write_room) { vita_write8_slow(addr, v); return; }
    vita_mem_host[at] = v;
}

#endif /* VITARECOMP_MEM_H */
