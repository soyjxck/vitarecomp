/* test_pool.c — the thread pool: every item once, no worker number shared. */

#include "../src/pool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

#define ITEMS 5000

typedef struct {
    unsigned char ran[ITEMS];               /* how often each item ran */
    volatile int  inside[64];               /* threads inside the function under each worker number */
    unsigned long sum[64];                  /* per-worker scratch, written without a lock */
    int           shared;                   /* set if two threads ever had one worker number at once */
} job;

static void work(void *ctx, unsigned item, unsigned worker) {
    job *j = ctx;
    assert(worker < vita_pool_size() && item < ITEMS);
    if (j->inside[worker]++) j->shared = 1;
    j->ran[item]++;
    /* long enough that the threads overlap */
    unsigned long x = item;
    for (int k = 0; k < 2000; k++) x = x * 1664525u + 1013904223u;
    j->sum[worker] += (x & 1) + item;
    j->inside[worker]--;
}

int main(void) {
    static job j;
    printf("pool: %u threads\n", vita_pool_size());
    assert(vita_pool_size() >= 1);
    for (int round = 0; round < 50; round++) {
        unsigned items = round % 7 == 0 ? 1 : (unsigned)(ITEMS - round * 13);
        unsigned long want = 0, got = 0;
        memset(&j, 0, sizeof(j));
        vita_pool_run(items, work, &j);
        for (unsigned i = 0; i < ITEMS; i++) assert(j.ran[i] == (i < items ? 1 : 0));
        assert(!j.shared);
        for (unsigned i = 0; i < items; i++) {
            unsigned long x = i;
            for (int k = 0; k < 2000; k++) x = x * 1664525u + 1013904223u;
            want += (x & 1) + i;
        }
        for (unsigned w = 0; w < 64; w++) got += j.sum[w];
        assert(got == want);
    }
    vita_pool_run(0, work, &j);             /* nothing to do is not an error */
    printf("  every item once, on its own worker ok\n");
    printf("all pool tests passed\n");
    return 0;
}
