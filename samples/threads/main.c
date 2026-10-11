/* threads — threads that wait for each other without asking the kernel.
 *
 * A console with several cores lets a thread wait by spinning: it reads a
 * word until another thread, running at the same time on another core,
 * changes it. Retail engines do this for their job queues and their locks.
 * A runtime that runs one guest thread at a time and switches only at
 * kernel waits never gets past the first of those loops, so this sample is
 * nothing but them:
 *
 *   - two workers spin on a gate the main thread opens;
 *   - they then take a test-and-set spinlock two thousand times each and
 *     add one to a counter inside it, with enough work under the lock that
 *     a time slice ends there and the other finds it held;
 *   - the main thread spins until both have said they are done.
 *
 * Nothing here calls a kernel wait. The picture is the result: a square
 * for each check, green when it held, and a bar as long as the counter.
 * It is the same picture however the threads were interleaved.
 *
 * Freestanding, like fbdemo. MIT, like the rest of this repository.
 */

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#define WIDTH   960
#define HEIGHT  544
#define PITCH   960
#define FB_SIZE 0x200000u

#define WORKERS 2
#define ROUNDS  2000

void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static volatile int      gate = 1;     /* the workers spin here until it opens */
static volatile int      lock;         /* a test-and-set spinlock               */
static volatile unsigned counter;      /* what the lock protects                */
static volatile int      done;         /* workers that have finished            */
static volatile int      passed[WORKERS];

static void spin_lock(volatile int *l) {
    while (__sync_lock_test_and_set(l, 1))
        while (*l) {}                  /* held: wait for the holder, then try again */
}

static void spin_unlock(volatile int *l) {
    __sync_lock_release(l);
}

static int worker(SceSize args, void *argp) {
    (void)args;
    int id = *(int *)argp;

    while (gate) {}                    /* only the main thread can end this */
    passed[id] = 1;

    for (int i = 0; i < ROUNDS; i++) {
        spin_lock(&lock);
        unsigned c = counter;
        for (volatile int k = 0; k < 200; k++) {}
        counter = c + 1;               /* wrong at once if the lock let two in */
        spin_unlock(&lock);
    }

    spin_lock(&lock);
    done++;
    spin_unlock(&lock);
    return 0;
}

static void rect(unsigned int *px, int x0, int y0, int w, int h, unsigned int colour) {
    for (int y = y0; y < y0 + h && y < HEIGHT; y++)
        for (int x = x0; x < x0 + w && x < WIDTH; x++)
            px[y * PITCH + x] = colour;
}

#define GREEN 0xFF40C040u
#define RED   0xFF4040E0u

int _start(SceSize args, void *argp) {
    (void)args; (void)argp;

    SceUID block = sceKernelAllocMemBlock("threads_fb",
                                          SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          FB_SIZE, NULL);
    if (block < 0) {
        sceKernelExitProcess(1);
        return 1;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    unsigned int *px = (unsigned int *)base;

    static int ids[WORKERS];
    int started = 0;
    for (int i = 0; i < WORKERS; i++) {
        ids[i] = i;
        SceUID t = sceKernelCreateThread("worker", worker, 0x10000100, 0x4000, 0, 0, NULL);
        if (t >= 0 && sceKernelStartThread(t, sizeof(int), &ids[i]) >= 0) started++;
    }

    rect(px, 0, 0, WIDTH, HEIGHT, 0xFF302820u);
    gate = 0;
    while (done != WORKERS) {}         /* only the workers can end this */

    rect(px,  64, 64, 96, 96, started == WORKERS ? GREEN : RED);
    rect(px, 192, 64, 96, 96, passed[0] && passed[1] ? GREEN : RED);
    rect(px, 320, 64, 96, 96, counter == WORKERS * ROUNDS ? GREEN : RED);
    rect(px, 448, 64, 96, 96, lock == 0 ? GREEN : RED);
    rect(px,  64, 224, (int)(counter * 800u / (WORKERS * ROUNDS)), 48, 0xFFFFFFFFu);

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size        = sizeof(fb);
    fb.base        = base;
    fb.pitch       = PITCH;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width       = WIDTH;
    fb.height      = HEIGHT;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
    }

    sceKernelFreeMemBlock(block);
    sceKernelExitProcess(0);
    return 0;
}
