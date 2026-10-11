/* sampler_win32.c — where the host's time goes. See sampler.h.
 *
 * A thread of its own stops each of the others for a moment, a thousand
 * times a second, and notes the address it was at. At the end the addresses
 * are named from the program's debug information: the functions the samples
 * fell in and, where a line is known, the lines. An inlined function's
 * samples go to the function it was inlined into and to its own lines,
 * which is the more useful half.
 *
 * A thread that waits is found in the system's wait call and says so by
 * name, so the list shows waiting and working side by side: the share of
 * each thread pool worker that was spent asleep is the first thing to read.
 *
 * Nothing is allocated while a thread is stopped: the stopped thread may
 * hold the allocator's lock.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <mmsystem.h>

#include "sampler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS    (1u << 18)         /* distinct addresses, a power of two */
#define THREADS  128u
#define ROWS     (1u << 14)         /* distinct functions, and distinct lines */
#define RELIST   256u               /* samples between two looks at which threads there are */

typedef struct { uint64_t address; uint32_t count; } slot;
typedef struct { char name[120]; uint32_t count; } row;

static slot   *g_slot;
static HANDLE  g_thread;
static volatile LONG g_quit, g_on;
static unsigned g_period;
static uint64_t g_samples, g_lost;

static void bump(uint64_t address) {
    uint32_t i = (uint32_t)((address * 0x9E3779B97F4A7C15ull) >> 46) & (SLOTS - 1);
    for (uint32_t n = 0; n < SLOTS; n++, i = (i + 1) & (SLOTS - 1)) {
        if (g_slot[i].address == address) { g_slot[i].count++; return; }
        if (!g_slot[i].address) { g_slot[i].address = address; g_slot[i].count = 1; return; }
    }
    g_lost++;
}

static unsigned threads_now(HANDLE *out) {
    DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 e;
    unsigned n = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    e.dwSize = sizeof(e);
    if (Thread32First(snap, &e)) {
        do {
            if (e.th32OwnerProcessID != process || e.th32ThreadID == self || n == THREADS) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, e.th32ThreadID);
            if (t) out[n++] = t;
        } while (Thread32Next(snap, &e));
    }
    CloseHandle(snap);
    return n;
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    static HANDLE threads[THREADS];
    unsigned count = 0, since = RELIST;
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    while (!g_quit) {
        Sleep(g_period);
        if (!g_on) continue;
        if (since++ >= RELIST) {
            for (unsigned i = 0; i < count; i++) CloseHandle(threads[i]);
            count = threads_now(threads);
            since = 0;
        }
        for (unsigned i = 0; i < count; i++) {
            CONTEXT c;
            if (SuspendThread(threads[i]) == (DWORD)-1) continue;
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(threads[i], &c)) {
#ifdef _M_ARM64
                bump(c.Pc);
#else
                bump(c.Rip);
#endif
                g_samples++;
            }
            ResumeThread(threads[i]);
        }
    }
    timeEndPeriod(1);
    for (unsigned i = 0; i < count; i++) CloseHandle(threads[i]);
    return 0;
}

int vita_sampler_start(unsigned period_ms) {
    if (g_thread) return 0;
    g_slot = calloc(SLOTS, sizeof(*g_slot));
    if (!g_slot) return 1;
    g_period = period_ms ? period_ms : 1;
    g_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    return g_thread ? 0 : 1;
}

void vita_sampler_run(int on) { g_on = on ? 1 : 0; }

static void add(row *table, const char *name, uint32_t count) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (uint32_t n = 0, i = h & (ROWS - 1); n < ROWS; n++, i = (i + 1) & (ROWS - 1)) {
        if (!table[i].count) snprintf(table[i].name, sizeof(table[i].name), "%s", name);
        else if (strcmp(table[i].name, name)) continue;
        table[i].count += count;
        return;
    }
}

static int by_count(const void *a, const void *b) {
    uint32_t x = ((const row *)a)->count, y = ((const row *)b)->count;
    return x < y ? 1 : x > y ? -1 : strcmp(((const row *)a)->name, ((const row *)b)->name);
}

static void print(FILE *f, row *table, unsigned top, uint64_t of, const char *what) {
    qsort(table, ROWS, sizeof(*table), by_count);
    fprintf(f, "  %s:\n", what);
    for (unsigned i = 0; i < top && table[i].count; i++)
        fprintf(f, "    %5.1f%%  %8u  %s\n", 100.0 * table[i].count / (double)(of ? of : 1), table[i].count, table[i].name);
}

/* A thread found in one of the system's ways of waiting was not at work. */
static int waits(const char *module, const char *name) {
    static const char *const WORDS[] = { "Wait", "Sleep", "Alert", "Delay", "Yield", "RemoveIoCompletion" };
    if (_stricmp(module, "ntdll") && _stricmp(module, "win32u") && _stricmp(module, "KERNELBASE")) return 0;
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++)
        if (strstr(name, WORDS[i])) return 1;
    return 0;
}

void vita_sampler_report(FILE *f) {
    if (!g_thread) return;
    g_quit = 1;
    WaitForSingleObject(g_thread, 2000);
    if (!g_samples) { fprintf(f, "vita_host: the sampler took no sample\n"); return; }

    HANDLE process = GetCurrentProcess();
    row *functions = calloc(ROWS, sizeof(row)), *lines = calloc(ROWS, sizeof(row));
    uint64_t waiting = 0;
    if (!functions || !lines) return;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, NULL, TRUE);

    for (uint32_t i = 0; i < SLOTS; i++) {
        union { SYMBOL_INFO s; char room[sizeof(SYMBOL_INFO) + 256]; } u;
        IMAGEHLP_MODULE64 module;
        IMAGEHLP_LINE64 line;
        DWORD64 offset = 0;
        DWORD column = 0;
        char name[400];
        if (!g_slot[i].count) continue;

        memset(&u, 0, sizeof(u));
        u.s.SizeOfStruct = sizeof(SYMBOL_INFO);
        u.s.MaxNameLen = 255;
        memset(&module, 0, sizeof(module));
        module.SizeOfStruct = sizeof(module);
        const char *in = SymGetModuleInfo64(process, g_slot[i].address, &module) ? module.ModuleName : "?";
        int named = SymFromAddr(process, g_slot[i].address, &offset, &u.s) != 0;
        if (named && waits(in, u.s.Name)) { waiting += g_slot[i].count; continue; }
        if (named) snprintf(name, sizeof(name), "%s!%s", in, u.s.Name);
        else snprintf(name, sizeof(name), "%s!?", in);
        add(functions, name, g_slot[i].count);

        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(process, g_slot[i].address, &column, &line) && line.FileName) {
            const char *base = line.FileName;
            for (const char *p = line.FileName; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
            snprintf(name, sizeof(name), "%s:%lu", base, (unsigned long)line.LineNumber);
            add(lines, name, g_slot[i].count);
        }
    }

    uint64_t working = g_samples - waiting;
    fprintf(f, "vita_host: %llu samples of the host's threads, one every %u ms each; %llu found a thread waiting, "
               "and the shares below are of the other %llu%s\n",
            (unsigned long long)g_samples, g_period, (unsigned long long)waiting, (unsigned long long)working,
            g_lost ? " (some addresses not kept)" : "");
    print(f, functions, 48, working, "by function, an inlined one counted with its caller");
    print(f, lines, 96, working, "by line, where the build has debug information (VITARECOMP_SYMBOLS)");
    SymCleanup(process);
    free(functions);
    free(lines);
}
