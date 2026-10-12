/* dispatch.h — indirect transfers, resolved at run time.
 *
 * A recompiled program has no program counter, so `BX r3` cannot be translated
 * into anything static: the destination is a value, and it is only known while
 * the program is running. Every recompiler needs this layer, and on Vita it
 * carries more traffic than most — the module has ~18,000 indirect call sites,
 * because `module_start` itself passes the real entry point as a POINTER rather
 * than calling it, and C++ virtual dispatch does the rest.
 *
 * The table maps a guest address to the C function that was recompiled from it.
 * A hit is a real transfer; a miss is a named trap, never a silent return —
 * jumping to an address we never translated has to stop the program, because
 * continuing means running the caller's code with the callee's job undone.
 */

#ifndef VITARECOMP_DISPATCH_H
#define VITARECOMP_DISPATCH_H

#include <stdint.h>

typedef void (*vita_fn)(void);

typedef struct {
    uint32_t addr;      /* guest virtual address, Thumb bit cleared */
    vita_fn  fn;
} vita_dispatch_entry;

/* The table must be sorted by addr; the generated one is. */
void vita_dispatch_init(const vita_dispatch_entry *table, uint32_t count);

/* Call the function recompiled from `addr`. The Thumb bit is masked off here
 * rather than at every call site — a pointer to Thumb code always has bit 0
 * set, and that is an instruction-set marker, not part of the address.
 *
 * `from` is the guest address of the transfer itself. A miss reports both, and
 * the destination alone is not enough to act on: "something jumped to
 * 0x814B9590" does not say which function to translate next, and that is the
 * only question a miss ever raises. */
void vita_dispatch(uint32_t from, uint32_t addr);

/* A function of the host's at a guest address of its own, for what the
 * runtime gives a program to call or starts a thread of its own at (the
 * movie player's, which runs a title's callbacks): a transfer to the
 * address calls `fn`. The address is one no module is loaded at, and the
 * same function asked for again has the same one. 0 when there is no room
 * for another. */
uint32_t vita_dispatch_host(vita_fn fn);

/* How many distinct addresses were dispatched to, and how many missed. Useful
 * for telling "the table is incomplete" from "the program went somewhere it
 * should not have". */
/* The start of the function an address falls in: the greatest table entry
 * not above it, or 0. For naming where a sample or a fault was. */
uint32_t vita_dispatch_floor(uint32_t addr);

uint32_t vita_dispatch_hits(void);
uint32_t vita_dispatch_misses(void);

#endif /* VITARECOMP_DISPATCH_H */
