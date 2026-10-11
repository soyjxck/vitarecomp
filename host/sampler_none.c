/* sampler_none.c — the build without a sampler of its own. Elsewhere than
 * Windows the system's profiler does the same job and needs no help. */

#include "sampler.h"

int  vita_sampler_start(unsigned period_ms) { (void)period_ms; return 1; }
void vita_sampler_run(int on) { (void)on; }
void vita_sampler_report(FILE *f) { (void)f; }
