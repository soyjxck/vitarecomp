/* pool.h — a few host threads for work that divides.
 *
 * The recompiled program runs one guest thread at a time, by design; the
 * host's other cores are idle while it does. Some of the runtime's own work
 * divides cleanly into pieces that touch different memory: the draws of a
 * scene, the tiles of what it draws to. This runs such pieces on a pool of
 * host threads while the caller, which is the guest thread that made the
 * call, takes part and waits. The guest sees a call that returned.
 */

#ifndef VITARECOMP_POOL_H
#define VITARECOMP_POOL_H

/* How many threads take part, the caller included: the host's processors
 * up to a limit, or what VITARECOMP_THREADS says. At least 1. */
unsigned vita_pool_size(void);

/* Call fn(ctx, item, worker) once for every item below `items`, on the
 * pool's threads and on the caller's, and return when all have returned.
 * `worker` is below vita_pool_size() and no two calls running at the same
 * time share one: it names per-thread scratch. Not re-entrant: fn must not
 * call this. */
void vita_pool_run(unsigned items, void (*fn)(void *ctx, unsigned item, unsigned worker), void *ctx);

#endif /* VITARECOMP_POOL_H */
