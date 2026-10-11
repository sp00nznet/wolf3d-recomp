/*
 * pool.c - spread a loop over the CPU's cores: the hi-res compositor's and
 * the DLSS pass's column loops (src/hires.c), where every column is drawn
 * independently. The game itself stays on one thread; only the host's
 * drawing of a frame, which the game waits for, is split.
 *
 * pool_for(n, fn, ctx) calls fn(i, ctx) for i in [0, n), in chunks taken by
 * whichever thread is free, and returns when all are done. Not reentrant.
 */
#include <windows.h>
#include "pool.h"

#define MAXT 32
static HANDLE threads[MAXT], go[MAXT], done[MAXT];
static int nthreads;                   /* workers, besides the calling thread */
static volatile LONG next_i;
static int job_n, job_chunk;
static void (*job_fn)(int, void *);
static void *job_ctx;

static void run_chunks(void)
{
    for (;;) {
        LONG i = InterlockedExchangeAdd(&next_i, job_chunk);
        if (i >= job_n) return;
        int end = i + job_chunk < job_n ? i + job_chunk : job_n;
        for (; i < end; i++) job_fn(i, job_ctx);
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    int k = (int)(INT_PTR)arg;
    for (;;) {
        WaitForSingleObject(go[k], INFINITE);
        run_chunks();
        SetEvent(done[k]);
    }
}

void pool_for(int n, void (*fn)(int i, void *ctx), void *ctx)
{
    static int init;
    if (!init) {
        init = 1;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        nthreads = (int)si.dwNumberOfProcessors - 1;   /* the game's thread draws too */
        if (nthreads > MAXT) nthreads = MAXT;
        for (int k = 0; k < nthreads; k++) {
            go[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
            done[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
            threads[k] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)k, 0, NULL);
        }
    }
    if (n <= 0) return;
    job_fn = fn; job_ctx = ctx; job_n = n;
    job_chunk = n / ((nthreads + 1) * 8);
    if (job_chunk < 4) job_chunk = 4;
    next_i = 0;
    int used = nthreads;
    if (n <= job_chunk) used = 0;                      /* too small to share */
    for (int k = 0; k < used; k++) SetEvent(go[k]);
    run_chunks();
    if (used) WaitForMultipleObjects(used, done, TRUE, INFINITE);
}
