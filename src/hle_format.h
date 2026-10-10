/* hle_format.h — the guest printf, shared by SceLibKernel's sceClib* family
 * and SceLibc's.
 *
 * A format string's arguments live in guest registers and guest memory, and
 * a %s is a guest address, so the format is interpreted here rather than
 * handed to the host's printf. The two callers differ only in where the
 * arguments start: a variadic call has them in r1-r3 and then the stack; a
 * va_list is a guest pointer into the stack, which the base AAPCS makes a
 * plain array of words with 64-bit values eight-byte aligned.
 */

#ifndef VITARECOMP_HLE_FORMAT_H
#define VITARECOMP_HLE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t next_reg;   /* the next argument register, 1-3; 4 means the stack */
    uint32_t sp_off;     /* the next stack argument, as an offset from sp      */
    uint32_t mem;        /* a va_list: read from here instead, when non-zero   */
} vita_vargs;

/* Formats the guest string at fmt_addr with the arguments into `out`, which
 * is always NUL-terminated within `cap`, and returns the length the whole
 * result would have had, as snprintf does. */
size_t vita_guest_vformat(char *out, size_t cap, uint32_t fmt_addr, vita_vargs *v);

/* The next 32-bit argument, from wherever the call left it. */
uint32_t vita_varg32(vita_vargs *v);

/* Scans the guest string at str_addr by the format at fmt_addr, writing
 * through the pointer arguments; returns the count assigned, or -1 when the
 * input ended before the first conversion. */
int vita_guest_vscanf(uint32_t str_addr, uint32_t fmt_addr, vita_vargs *v);

#endif /* VITARECOMP_HLE_FORMAT_H */
