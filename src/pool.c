/* pool.c — a few host threads for work that divides. See pool.h.
 *
 * A job is a count of items and a function. The threads and the caller
 * take items one at a time until none is left, and the caller returns when
 * the last has finished. An item is taken by adding one to a counter, with
 * no lock: a job can be four hundred items of a few microseconds each, and
 * two dozen threads queuing on a lock for every one of those spent more
 * time in the queue than at the work. The lock is for the two things that
 * happen once a job to each thread, joining it and leaving it, and for
 * sleeping.
 *
 * Nobody waits for a thread that is still asleep: one that wakes late finds
 * nothing to take. Jobs come in bursts, a few dozen in a frame and then
 * none for a while, and waking a sleeping thread costs more than most items
 * take; so a thread that runs out of work watches for the next job for a
 * moment before it sleeps.
 */

#include "pool.h"

#include <stdlib.h>
#include <threads.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#define relax() _mm_pause()
#else
#define relax() ((void)0)
#endif

/* One more, and what it was before; one fewer, and what it is now. */
#if defined(_MSC_VER)
#include <intrin.h>
#define count_up(p)   ((unsigned)_InterlockedIncrement((volatile long *)(p)) - 1u)
#define count_down(p) ((unsigned)_InterlockedDecrement((volatile long *)(p)))
#else
#define count_up(p)   __atomic_fetch_add((p), 1u, __ATOMIC_SEQ_CST)
#define count_down(p) __atomic_sub_fetch((p), 1u, __ATOMIC_SEQ_CST)
#endif

#define POOL_MAX 32
#define WATCH    20000              /* looks at the job counter before sleeping: a millisecond or so */

static struct {
    mtx_t    lock;
    cnd_t    work, done;            /* a job was posted; its last item finished and every thread has left it */
    unsigned size;                  /* threads taking part, the caller included */

    /* the job in hand: set under the lock before it is posted, not changed until it is over */
    unsigned items;
    void   (*fn)(void *, unsigned, unsigned);
    void    *ctx;
    volatile unsigned next;         /* the first item not yet taken: counted up without the lock */
    volatile unsigned left;         /* items not yet finished: counted down without it */
    unsigned inside;                /* pool threads that have joined the job and not left: under the lock */
} g;

/* Goes up by one a job. Read without the lock by a thread deciding whether
 * to look properly; a stale reading costs it one more look and nothing else. */
static volatile unsigned g_posted;

static unsigned processors(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (unsigned)info.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#endif
}

unsigned vita_pool_size(void) {
    if (!g.size) {
        const char *e = getenv("VITARECOMP_THREADS");
        unsigned n = e ? (unsigned)strtoul(e, NULL, 0) : 0;
        if (!n) {
            /* Three quarters of the processors. The rest are for the guest's
             * own threads and the host's window, and on a machine whose
             * processors are two to a core, or fast ones and slow ones, the
             * last quarter adds little: thirty threads on thirty-two
             * processors drew a frame slower than twenty-four. */
            n = processors();
            n = n > 2 ? n * 3 / 4 : 1;
        }
        g.size = n > POOL_MAX ? POOL_MAX : n;
    }
    return g.size;
}

/* Take items until there are none. The job cannot change under a thread
 * that is here: the caller of vita_pool_run does not return, and so posts
 * nothing new, while any thread is inside. */
static void take(unsigned worker) {
    unsigned items = g.items;
    for (;;) {
        unsigned item = count_up(&g.next);
        if (item >= items) break;
        g.fn(g.ctx, item, worker);
        count_down(&g.left);
    }
}

static int worker_main(void *arg) {
    unsigned worker = (unsigned)(size_t)arg, seen = 0;
    mtx_lock(&g.lock);
    for (;;) {
        /* a job this thread has not looked at, with something left to take: join it */
        if (g_posted != seen) {
            seen = g_posted;
            if (g.next < g.items) {
                g.inside++;
                mtx_unlock(&g.lock);
                take(worker);
                mtx_lock(&g.lock);
                if (--g.inside == 0 && g.left == 0) cnd_broadcast(&g.done);
                continue;
            }
        }
        /* nothing: watch for the next job a while, then sleep until one is posted */
        mtx_unlock(&g.lock);
        for (int i = 0; i < WATCH && g_posted == seen; i++) relax();
        mtx_lock(&g.lock);
        while (g_posted == seen) cnd_wait(&g.work, &g.lock);
    }
    return 0;
}

void vita_pool_run(unsigned items, void (*fn)(void *ctx, unsigned item, unsigned worker), void *ctx) {
    static int ready;
    unsigned size = vita_pool_size();
    if (!items) return;
    if (size == 1 || items == 1) {
        for (unsigned i = 0; i < items; i++) fn(ctx, i, 0);
        return;
    }
    if (!ready) {
        ready = 1;
        mtx_init(&g.lock, mtx_plain);
        cnd_init(&g.work);
        cnd_init(&g.done);
        for (unsigned w = 1; w < size; w++) {
            thrd_t t;
            if (thrd_create(&t, worker_main, (void *)(size_t)w) != thrd_success) break;
            thrd_detach(t);
        }
    }
    mtx_lock(&g.lock);
    g.fn = fn;
    g.ctx = ctx;
    g.items = items;
    g.next = 0;
    g.left = items;
    g_posted++;
    cnd_broadcast(&g.work);
    mtx_unlock(&g.lock);

    take(0);                        /* the caller is worker 0 */

    /* the last items are in other hands: they are short, so look a few times before sleeping on it */
    for (int i = 0; i < WATCH && g.left; i++) relax();
    mtx_lock(&g.lock);
    while (g.left || g.inside) cnd_wait(&g.done, &g.lock);
    mtx_unlock(&g.lock);
}
