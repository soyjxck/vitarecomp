/* sched.h — guest threads on the host.
 *
 * The recompiled code keeps its registers in globals (cpu.h) and has no idea
 * which thread it is on; that is what makes it fast and what makes threads a
 * runtime matter. The arrangement here: one host thread per guest thread,
 * exactly one of them running guest code at any moment, and a switch where
 * the console's kernel would block or yield (a wait on a semaphore, mutex
 * or condition that cannot be satisfied, a delay, a wait for a thread to
 * end, the vertical blank) or where the running thread's time slice ends,
 * which is always at a backward branch of the generated code.
 *
 * At a switch the running thread saves the register globals into its own
 * record, loads the next thread's, hands it the baton and sleeps on its own
 * condition variable until someone hands the baton back. Nothing else is
 * ever concurrent, so the generated code's assumptions hold as they did
 * with one thread: a LDREX/STREX pair cannot be interrupted, memory needs no
 * ordering, and the exclusive monitor is never broken.
 *
 * The kernel objects (hle_threadmgr.c, hle_ult.c) express their waits as a
 * kind and an object address; a signal wakes the waiters on that pair, and
 * the woken thread re-checks the condition it waited for, since another
 * thread may have taken it first.
 */

#ifndef VITARECOMP_SCHED_H
#define VITARECOMP_SCHED_H

#include <stdint.h>
#include <stdio.h>

enum {
    VW_NONE = 0,
    VW_SEMA,        /* a kernel semaphore, by uid                     */
    VW_MUTEX,       /* a kernel mutex by uid, or a lightweight one by work address */
    VW_COND,        /* a condition variable, by uid or work address   */
    VW_THREAD_END,  /* a thread's end, by its uid                      */
    VW_DELAY,       /* a delay: nothing wakes it but the clock         */
    VW_ULT,         /* a SceUlt object, by its address                 */
    VW_EVENT_FLAG,  /* an event flag, by uid                           */
    VW_MSG_PIPE,    /* a message pipe, by uid: room in it, or bytes    */
};

/* The uid of the running thread; the main thread's before any is made. */
uint32_t vita_thread_self(void);

/* The kernel's thread lifecycle. Returns 0 or a negative error; create
 * returns the uid (positive) or 0. */
uint32_t vita_thread_create(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size);
int      vita_thread_start(uint32_t uid, uint32_t arglen, uint32_t argp);
int      vita_thread_wait_end(uint32_t uid, uint32_t timeout_us, int *status);
int      vita_thread_delete(uint32_t uid);
void     vita_thread_exit(int status);          /* the running thread; never returns */

/* The running thread's record, for what the kernel keeps per thread: its
 * TLS block (written by the thread manager on first use). */
uint32_t *vita_thread_tls_slot(uint32_t uid);

/* For sceKernelGetThreadInfo. Returns 0 when the uid is a thread. */
int vita_thread_info(uint32_t uid, char *name, uint32_t cap, uint32_t *entry,
                     uint32_t *stack, uint32_t *stack_size, uint32_t *prio);

/* Block the running thread on (kind, obj) until woken or, with a non-zero
 * timeout in microseconds, until it passes. Returns 0 when woken, -1 on
 * the timeout. */
int  vita_sched_block(int kind, uint32_t obj, uint32_t timeout_us);

/* Make the threads waiting on (kind, obj) runnable: one, or all. */
void vita_sched_wake(int kind, uint32_t obj, int all);

/* Callbacks (hle_threadmgr.c): a notice for one, by its uid, and the run of
 * those the running thread has notices for, which a wait that allows
 * callbacks (the ...CB forms) does on its way. */
int  vita_callback_notify(uint32_t uid, uint32_t arg);
void vita_callbacks_run(void);

/* The display's blank callbacks given their notice if the clock has moved
 * into another sixtieth of a second since they last had one
 * (hle_display.c): for a wait that is not for a blank and lets callbacks
 * run. */
void vita_display_poll(void);

/* Let another runnable thread go, if there is one. */
void vita_sched_yield(void);

/* Preemption, driven by the generated code (recomp_rt.h): vita_slice_end
 * when the running thread's count of backward branches runs out, and
 * vita_spin_wait in a loop that only another thread can end. */

/* Sleep for the given microseconds; other threads run meanwhile. */
void vita_sched_delay(uint32_t us);

/* Something a host thread holds that the next thread to run may want and
 * cannot take from it: under OpenGL a graphics card's context belongs to
 * the thread it was made current on until that thread lets it go
 * (gpu_gl.c). `leave` is called on the running thread each time it is
 * about to hand the baton to another or to end, while the baton is still
 * its own; what it lets go of, the next thread to want it takes up for
 * itself. One such thing at a time: a second call replaces the first, and
 * NULL is none. */
void vita_sched_on_handover(void (*leave)(void));

/* The thread holding the baton, for a sampler on another host thread: its
 * uid and name, or 0 and "(idle)" while every guest thread waits. */
uint32_t vita_sched_running(char *name, uint32_t cap);

/* How many threads exist and how many are runnable, for the host's summary. */
uint32_t vita_sched_threads(uint32_t *runnable);

/* Every thread on a line: its name, what it is doing or waiting on, and the
 * guest address it was last at (which needs C emitted with
 * VITARECOMP_EMIT_PC). A program that has stopped moving is usually a
 * thread waiting for something nobody will give it, and this says which
 * thread and what. */
void vita_sched_dump(FILE *f);

#endif /* VITARECOMP_SCHED_H */
