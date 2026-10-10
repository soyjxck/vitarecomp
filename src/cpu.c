/* cpu.c — the machine state the generated C operates on. */

#include "vitarecomp/cpu.h"

#include <stdio.h>

uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
uint32_t r8, r9, r10, r11, r12;
uint32_t sp, lr;

int flag_n, flag_z, flag_c, flag_v;
int flag_ge;

uint32_t vita_last_pc;
int      vita_trace_insns;
uint32_t vita_trace_lo, vita_trace_hi = 0xFFFFFFFFu;

void vita_trace_insn(uint32_t addr) {
    fprintf(stderr,
            "TRACE pc  0x%08X  r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X"
            " r8=%08X ip=%08X sp=%08X lr=%08X %c%c%c%c\n",
            addr, r0, r1, r2, r3, r4, r5, r6, r7, r8, r12, sp, lr,
            flag_n ? 'N' : 'n', flag_z ? 'Z' : 'z', flag_c ? 'C' : 'c', flag_v ? 'V' : 'v');
}
