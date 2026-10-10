/* cpu.h — the recompiled program's machine state.
 *
 * Registers are globals rather than a struct passed around, for the same
 * reason every recompiler does it: the generated C is millions of lines and
 * `r0` reads better than `cpu->r[0]` a few hundred thousand times over. There
 * is one thread of recompiled control at a time.
 */

#ifndef VITARECOMP_CPU_H
#define VITARECOMP_CPU_H

#include <stdint.h>

/* r0-r12, then the three with architectural meaning. r15 (PC) is deliberately
 * absent: a recompiled program has no program counter, and any instruction
 * that genuinely reads PC has its value resolved at translation time. */
extern uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
extern uint32_t r8, r9, r10, r11, r12;
extern uint32_t sp, lr;

/* NZCV, held as separate ints rather than packed into a CPSR word.
 *
 * ARM leans on these far harder than MIPS does — a comparison and its branch
 * are separate instructions with arbitrary distance between them, and carry
 * feeds ADC/SBC chains. Keeping them unpacked means the compiler can usually
 * fold a flag write into the operation that produced it, and it makes the
 * generated C readable. */
extern int flag_n, flag_z, flag_c, flag_v;

/* The four GE bits, written by the byte- and halfword-parallel arithmetic
 * (UADD8, USUB8, ...) and read by SEL. Bit i covers byte i. They are what
 * lets the C library's strlen test four bytes at a time. */
extern int flag_ge;

/* --- the bring-up hook ------------------------------------------------------
 *
 * There is no program counter in a recompiled program, which is right until
 * something goes wrong and the question is "where". C emitted with
 * VITARECOMP_EMIT_PC=1 set in the environment of `armrecomp emit` records
 * each instruction's guest address here before executing it, so a bad access
 * or a trap can say which instruction it was; with vita_trace_insns set and
 * a window, it also logs the registers. Off, it is one store per
 * instruction; absent (the default emit), it costs nothing. */
extern uint32_t vita_last_pc;
extern int      vita_trace_insns;
extern uint32_t vita_trace_lo, vita_trace_hi;   /* the window, inclusive */
void vita_trace_insn(uint32_t addr);

static inline void vita_trace_pc(uint32_t addr) {
    vita_last_pc = addr;
    if (vita_trace_insns && addr >= vita_trace_lo && addr <= vita_trace_hi)
        vita_trace_insn(addr);
}

#endif /* VITARECOMP_CPU_H */
