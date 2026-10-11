/* sampler.h — where the host's time goes.
 *
 * The runtime's own profile (src/profile.c) answers in guest addresses:
 * which function of the title the time is in. This one answers in host
 * functions and lines, for the time that is not the title's at all: the
 * renderer's, mostly. `VITARECOMP_HOST_PROFILE=MS[@FROM-TO]` on vita_host
 * samples every thread each MS milliseconds, between two vertical blanks if
 * they are given, and prints the tally at the end. The lines want a build
 * configured with -DVITARECOMP_SYMBOLS=ON.
 */

#ifndef VITA_HOST_SAMPLER_H
#define VITA_HOST_SAMPLER_H

#include <stdio.h>

/* Nonzero if this build has no sampler. Sampling waits for vita_sampler_run. */
int  vita_sampler_start(unsigned period_ms);
void vita_sampler_run(int on);
void vita_sampler_report(FILE *f);

#endif
