/* clock.h — the one clock everything in the runtime reads.
 *
 * A title asks the time in a dozen ways: the process clock, the real-time
 * clock, the C library's, the timeouts of its waits. They all come from
 * here, so that they agree with each other, and so that the host can
 * choose what time is. By default it is the host's clock. A host that
 * wants a run to repeat exactly asks for a virtual one (host.h), which
 * moves only with what the program does: a sixtieth of a second at each
 * vertical blank, the length of a wait when every thread is waiting, and a
 * microsecond each time it is read, so that a loop spinning on the clock
 * still sees it move.
 */

#ifndef VITARECOMP_CLOCK_H
#define VITARECOMP_CLOCK_H

#include <stdint.h>

/* Microseconds since the clock was first read. */
uint64_t vita_clock_us(void);

/* Microseconds since 1970, UTC. The virtual clock starts at midnight on the
 * first of January 2026. */
uint64_t vita_clock_unix_us(void);

/* Let this much time pass: the host sleeps, or the virtual clock moves on. */
void vita_clock_wait(uint64_t us);

/* A vertical blank has passed. */
void vita_clock_vblank(void);

/* Whether the clock is the virtual one: for whatever would let the host's
 * own time into a run that is meant to repeat. */
int vita_clock_is_virtual(void);

#endif /* VITARECOMP_CLOCK_H */
