/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all #includes. */
#define _GNU_SOURCE

#include "win32_compat.h"

#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#else
#include <sys/sysinfo.h>
#endif

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    /*
     * RECOMP_LOCK_FAIR=1: hand the lock to whoever has been waiting longest.
     *
     * Darwin's default mutex policy lets a thread that releases a lock and
     * immediately asks for it again jump the queue. That is usually the right
     * trade -- it saves a context switch -- and it is the wrong one for the
     * guest scheduler lock, which is released and re-taken every time slice
     * by whichever thread is running. Under that pattern a barging mutex can
     * leave a waiting thread waiting more or less indefinitely, and a
     * sampling profile found the game's main thread doing exactly that for
     * every one of twelve seconds' worth of samples while other guest threads
     * ran.
     *
     * Behind a switch and off by default until it is measured, because
     * fairness is not free: every handover becomes a real context switch.
     */
    { static int fair = -1;
      if (fair < 0) fair = getenv("RECOMP_LOCK_FAIR") ? 1 : 0;
#if defined(__APPLE__) && defined(PTHREAD_MUTEX_POLICY_FAIRSHARE_NP)
      if (fair)
          pthread_mutexattr_setpolicy_np(&attr,
                                         PTHREAD_MUTEX_POLICY_FAIRSHARE_NP);
#else
      (void)fair;
#endif
    }
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockSemaphore = m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
        pthread_cond_init(c, NULL);
        /* Race window OK for typical Win32 usage (the CS is held). */
        cv->Ptr = c;
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = (pthread_mutex_t *)cs->LockSemaphore;
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE } w32_kind;

#define W32_MAX_APC 16

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* event */
    int             signaled;
    int             manual_reset;

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;
    int             thread_joinable;
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    volatile int    suspend_count;
    pthread_cond_t  gate;
    /* Parked in SuspendThread right now, and since when: a resume waits for
     * the parked thread to leave, and the stall report needs to say which
     * thread has been parked and for how long. */
    volatile int    parked;
    long long       park_since;
    pthread_cond_t  left_gate;
    /* A wake-up that arrived before the park it was meant for. Separate from
     * suspend_count on purpose -- see ResumeThread. */
    volatile int    pending_resume;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer */
    int             timer_cancel;
    DWORD           timer_due;
    DWORD           timer_period;
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)
#define STILL_ACTIVE 259u

static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
static volatile LONG        s_next_tid = 1000;

DWORD GetCurrentThreadId(void)
{
    if (t_tid == 0)
        t_tid = (DWORD)InterlockedIncrement(&s_next_tid);
    return t_tid;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static w32_object *obj_alloc(w32_kind kind);
/* Threads not created through CreateThread (the host's main thread) get a
 * thread object on first use, so the self-suspend hand-off and priority
 * tracking work from them too. */
static w32_object *self_obj(void)
{
    if (!t_self_obj) {
        w32_object *o = obj_alloc(K_THREAD);
        o->thread   = pthread_self();
        o->refcount = 1;
        t_self_obj  = o;
    }
    return t_self_obj;
}
HANDLE GetCurrentThread(void)   { return (HANDLE)self_obj(); }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

/* Every object this layer has allocated.
 *
 * A HANDLE crossing back from the guest is a 32-bit value the guest kept, and
 * the bridge passes an unrecognised one through as though it were a pointer.
 * Dereferencing it then faults inside this file on an address that belongs to
 * the guest, not the host -- SuspendThread reading ->kind off 0x00050790,
 * which is a guest RAM address, is what that looks like. The title is not
 * doing anything wrong: it has a value it believes is a thread, and the
 * translation lost it somewhere upstream.
 *
 * Upstream is worth fixing on its own, but no amount of fixing it makes
 * dereferencing an arbitrary integer safe. So every object is registered when
 * it is created, and a handle is only followed if it is in the set. A stale
 * or bogus handle then returns an error the way Win32 would, instead of
 * taking the process down.
 *
 * A flat open-addressed set: allocation is rare, lookup is on every handle
 * use, and the whole thing has to work while several guest threads are inside
 * it at once. */
#define W32_OBJ_SET 8192
static void *g_obj_set[W32_OBJ_SET];
static pthread_mutex_t g_obj_set_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t obj_hash(const void *p)
{
    uintptr_t v = (uintptr_t)p >> 4;
    v ^= v >> 17; v *= 0x9E3779B1u; v ^= v >> 13;
    return (size_t)(v & (W32_OBJ_SET - 1));
}

static void obj_set_add(void *p)
{
    size_t i, h = obj_hash(p);
    pthread_mutex_lock(&g_obj_set_lock);
    for (i = 0; i < W32_OBJ_SET; i++) {
        size_t k = (h + i) & (W32_OBJ_SET - 1);
        if (!g_obj_set[k] || g_obj_set[k] == p) { g_obj_set[k] = p; break; }
    }
    pthread_mutex_unlock(&g_obj_set_lock);
}

/* Is this handle one of ours at all? NULL if not. */
static w32_object *obj_any(HANDLE h)
{
    size_t i, hh;
    w32_object *found = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    hh = obj_hash((void *)h);
    pthread_mutex_lock(&g_obj_set_lock);
    for (i = 0; i < W32_OBJ_SET; i++) {
        size_t k = (hh + i) & (W32_OBJ_SET - 1);
        if (!g_obj_set[k]) break;
        if (g_obj_set[k] == (void *)h) { found = (w32_object *)h; break; }
    }
    pthread_mutex_unlock(&g_obj_set_lock);
    return found;
}

/* Is this handle one of ours, and of the kind expected? NULL if not. */
static w32_object *obj_check(HANDLE h, w32_kind kind)
{
    size_t i, hh;
    w32_object *found = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    hh = obj_hash((void *)h);
    pthread_mutex_lock(&g_obj_set_lock);
    for (i = 0; i < W32_OBJ_SET; i++) {
        size_t k = (hh + i) & (W32_OBJ_SET - 1);
        if (!g_obj_set[k]) break;
        if (g_obj_set[k] == (void *)h) { found = (w32_object *)h; break; }
    }
    pthread_mutex_unlock(&g_obj_set_lock);
    if (found && found->kind != kind) return NULL;
    return found;
}

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    pthread_cond_init(&o->gate, NULL);
    pthread_cond_init(&o->left_gate, NULL);
    obj_set_add(o);
    return o;
}

static void obj_release(w32_object *o)
{
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
    } else if (o->kind == K_FILEMAP) {
        if (o->fd >= 0) close(o->fd);
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return (HANDLE)o;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = obj_any(h);
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = obj_any(h);
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    { w32_object *o = obj_any(h); if (o) obj_release(o); }
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = src; return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = (w32_object *)src;
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
    return TRUE;
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = t_self_obj;
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;

    for (;;) {
        int ready = 0;
        switch (o->kind) {
        case K_EVENT:  ready = o->signaled; break;
        case K_THREAD: ready = o->exited;   break;
        case K_SEM:    ready = (o->sem_count > 0); break;
        case K_MUTEX:
            ready = (o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId());
            break;
        default:       ready = 1; break;
        }
        if (ready) break;

        int rc = timed ? pthread_cond_timedwait(&o->cond, &o->lock, &ts)
                       : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT) { result = WAIT_TIMEOUT; break; }
    }

    if (result == WAIT_OBJECT_0) {
        switch (o->kind) {
        case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
        case K_SEM:   o->sem_count--; break;
        case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    { w32_object *o = obj_any(h);
      return o ? wait_single(o, ms) : WAIT_FAILED; }
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return WaitForSingleObject(h, ms);
}

/*
 * WaitForMultipleObjects: polling implementation. Adequate for the light
 * multi-object waits the Xbox kernel HLE issues; not a high-throughput path.
 */
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    for (;;) {
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD got = 0;
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) got++;
            if (got == count) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                    return WAIT_OBJECT_0 + i;
        }

        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec ||
                (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                return WAIT_TIMEOUT;
        }
        usleep(1000);
    }
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return (HANDLE)o;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    t_self_obj = o;
    t_tid      = o->tid;

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;

    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize)
        pthread_attr_setstacksize(&attr, stackSize < 65536 ? 65536 : stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    o->thread_joinable = 1;

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
    }
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* A resume that arrives before its suspend is remembered, not discarded.
 *
 * Win32 clamps the suspend count at zero, so a ResumeThread with nothing to
 * resume does nothing. That is faithful, and on this host it is a hang: the
 * title parks a worker with SuspendThread(GetCurrentThread()) and wakes it
 * with ResumeThread from another thread, and the console's single CPU makes
 * the order reliable in a way that a real multi-core host does not. When the
 * resume lands first it is swallowed, the worker then suspends into a wake-up
 * that already happened, and everything waiting on it waits forever -- which
 * is what a thread sitting inside NtSuspendThread while another spins on a
 * critical section ninety-eight thousand times looks like.
 *
 * So the count may go to -1: one pending resume, which the next suspend
 * consumes instead of blocking. Bounded at one because this is a hand-off,
 * not a counter -- letting it run further negative would turn a title that
 * genuinely resumes more often than it suspends into one that can never
 * suspend at all.
 */
/* ── the suspend/resume hand-off, recorded ────────────────────────────────
 *
 * A title that parks a worker with SuspendThread(GetCurrentThread()) and wakes
 * it with ResumeThread from another thread is relying on the console's single
 * CPU to order the two. On a real multi-core host the order is not guaranteed,
 * and when it goes wrong the symptom is a permanent stall with one thread
 * inside NtSuspendThread and another spinning on a critical section several
 * hundred thousand times.
 *
 * The bank of one pending resume (see ResumeThread) fixes the simple race.
 * When it is not enough, the only way to say what happened is to have kept the
 * last few hand-offs -- so keep them, and print them when a park has clearly
 * gone unanswered. Sixty-four entries is a few hundred bytes and covers far
 * more history than the stall needs.
 */
#define W32_HANDOFF_LOG 64
typedef struct {
    long long ms;
    DWORD     by;          /* thread that made the call */
    DWORD     target;      /* thread it acted on */
    char      op;          /* S park, s suspend-other, R resume, r banked */
    int       count;       /* suspend count afterwards */
} w32_handoff;
static w32_handoff     g_handoff[W32_HANDOFF_LOG];
static int             g_handoff_n;
static pthread_mutex_t g_handoff_lock = PTHREAD_MUTEX_INITIALIZER;

static void handoff_note(char op, DWORD target, int count)
{
    pthread_mutex_lock(&g_handoff_lock);
    g_handoff[g_handoff_n % W32_HANDOFF_LOG].ms     = (long long)GetTickCount64();
    g_handoff[g_handoff_n % W32_HANDOFF_LOG].by     = GetCurrentThreadId();
    g_handoff[g_handoff_n % W32_HANDOFF_LOG].target = target;
    g_handoff[g_handoff_n % W32_HANDOFF_LOG].op     = op;
    g_handoff[g_handoff_n % W32_HANDOFF_LOG].count  = count;
    g_handoff_n++;
    pthread_mutex_unlock(&g_handoff_lock);
}

/* Printed by the stall report. Oldest first, newest last. */
void w32_handoff_report(void)
{
    int i, first, n;
    long long now = (long long)GetTickCount64();
    pthread_mutex_lock(&g_handoff_lock);
    n = g_handoff_n < W32_HANDOFF_LOG ? g_handoff_n : W32_HANDOFF_LOG;
    first = g_handoff_n < W32_HANDOFF_LOG ? 0 : g_handoff_n % W32_HANDOFF_LOG;
    fprintf(stderr, "  [HANDOFF] last %d suspend/resume calls "
                    "(S=parked self, s=suspended another, R=resumed, "
                    "r=resume banked for a suspend that had not happened):\n", n);
    for (i = 0; i < n; i++) {
        w32_handoff *e = &g_handoff[(first + i) % W32_HANDOFF_LOG];
        fprintf(stderr, "  [HANDOFF]   -%6lldms  tid %lu  %c  thread %lu"
                        "  count now %d\n",
                now - e->ms, (unsigned long)e->by, e->op,
                (unsigned long)e->target, e->count);
    }
    pthread_mutex_unlock(&g_handoff_lock);
    fflush(stderr);
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_check(h, K_THREAD);
    if (!o) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->suspend_count > 0) {
        if (--o->suspend_count == 0) {
            pthread_cond_broadcast(&o->gate);
            /*
             * Wait for the parked thread to actually leave the gate.
             *
             * On the console a resume is a hand-off: one CPU, and the thread
             * being woken runs before the thread that woke it does anything
             * else. Here both are real threads on real cores, so without this
             * the resumer can run all the way round its loop and issue a
             * second resume before the first has been consumed -- and the
             * second one lands on a thread whose count is already zero, where
             * it is banked (once) or lost. Bounded, because a stall is better
             * than a hang if this reasoning is ever wrong.
             */
            /* Polled rather than waited on, because there are two kinds of
             * park and only one of them can signal: a thread that parked
             * itself is in pthread_cond_wait and will signal left_gate, but a
             * thread stopped by the suspend signal is inside a signal handler,
             * where signalling a condition variable is not safe. Both clear
             * ->parked, so watch that. Bounded: a stall is better than a hang
             * if this reasoning is ever wrong. */
            { int spins = 0;
              while (o->parked && spins++ < 2000) {
                  pthread_mutex_unlock(&o->lock);
                  sched_yield();
                  pthread_mutex_lock(&o->lock);
              } }
        }
    } else {
        /* Nothing to resume yet: bank the wake-up for the park it was meant
         * for. Kept in its own field, not as a negative suspend count.
         *
         * Sharing the field was a bug with teeth: once the count went to -1,
         * every later SuspendThread on that thread consumed the bank instead
         * of suspending, and every ResumeThread put it back -- so a title that
         * drives a worker with alternating Suspend and Resume, which is what
         * JSRF's loader does, could never suspend that worker again. The
         * hand-off log shows exactly that, four Suspend/Resume pairs in one
         * millisecond with the count flipping -1, 0, -1, 0 and the worker
         * never once stopping.
         *
         * Counted, not a flag. There are two producer threads feeding this
         * title's loader worker, and with a flag a second wake-up arriving
         * before the worker parks was silently merged into the first -- so the
         * worker parked once too often and the item that second wake-up was
         * announcing waited for whatever came next. That shows up as a loading
         * screen that needs thirteen thousand frames where the console needs
         * fifteen hundred. Skipping a park the title did not strictly need
         * costs one wasted trip round the worker's loop; missing one costs the
         * item. The cap keeps a title that resumes far more often than it
         * suspends from being unable to park at all. */
        if (o->pending_resume < 16) o->pending_resume++;
        pthread_mutex_unlock(&o->lock);
        handoff_note('r', o->tid, 0);
        return prev;
    }
    { int c = o->suspend_count;
      pthread_mutex_unlock(&o->lock);
      handoff_note('R', o->tid, c); }
    return prev;
}

/* ── suspending a thread other than the caller ────────────────────────────
 *
 * POSIX has no way to stop a running thread from outside, so this used to
 * track the count and let the target keep running. That is not a small gap:
 * JSRF's loader uses SuspendThread/ResumeThread on its worker threads as a
 * control mechanism, and a worker that does not stop when it is told to runs
 * its queue in an order the title never expects. The symptom is the loading
 * screen never ending, with two threads waiting on an event and a third
 * parked with nobody left to wake it.
 *
 * So the target stops itself instead, at the next checkpoint. The recompiler
 * already calls a hook on entry to every recompiled function, which is a few
 * microseconds apart in practice -- close enough to "immediately" for a title
 * that suspends a worker to keep it away from shared state, and cooperative,
 * which means no signals and no stopping a thread in the middle of a malloc.
 *
 * The checkpoint runs with the guest lock released, always: a thread that
 * parked while holding the lock that serialises guest execution would take
 * every other guest thread down with it.
 */
/* Stopping a thread that never reaches a checkpoint.
 *
 * The cooperative checkpoint below runs on entry to every recompiled function,
 * which is often enough for any thread that is calling functions. JSRF has one
 * that is not: a worker whose whole body is a loop with no calls in it, which
 * the title suspends and resumes constantly. Under a cooperative scheme that
 * thread never stops -- and a title that suspends a thread is usually doing it
 * to keep that thread away from state it is about to touch, so not stopping is
 * not a missing optimisation, it is a data race. A wild pointer several
 * hundred thousand instructions later is what that looks like.
 *
 * So: a signal, which interrupts the thread wherever it is, and a handler that
 * waits for the count to fall. The wait is a spin with sched_yield rather than
 * a condition variable because almost nothing is safe to call from a signal
 * handler and sched_yield is; a suspended guest thread burning a core for the
 * few hundred microseconds a title holds a suspend is a fair trade for the
 * suspend meaning what it says. SA_RESTART so that a thread signalled inside a
 * blocking call resumes it rather than failing with EINTR.
 */
#define W32_SUSPEND_SIGNAL SIGUSR2

static void w32_suspend_signal(int sig)
{
    w32_object *o = t_self_obj;
    struct timespec t0, now;
    (void)sig;
    if (!o) return;
    clock_gettime(CLOCK_MONOTONIC, &t0);   /* async-signal-safe, unlike most */
    o->parked = 1;
    while (o->suspend_count > 0) {
        sched_yield();
        /*
         * A safety valve, and an admission.
         *
         * Making a suspend real means the target genuinely stops, and a
         * thread that stops and is never resumed stops the whole title --
         * which is worse than the gap it was fixing, because before this the
         * target simply carried on. A resume can go missing here for reasons
         * that are the emulation's fault rather than the title's, and there
         * is no way to be sure it cannot.
         *
         * So: honour the suspend for as long as any plausible suspend lasts,
         * and then carry on rather than hang. A title whose worker resumes a
         * quarter of a second late sees a thread that was stopped for a
         * quarter of a second, which is the emulation being slow. A title
         * whose resume is lost sees the behaviour it had yesterday. Neither
         * is a frozen game.
         *
         * write() rather than fprintf: almost nothing is safe to call from a
         * signal handler, and this one already spends its time in sched_yield
         * for the same reason.
         */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) * 1000000000LL
          + (now.tv_nsec - t0.tv_nsec) > 250000000LL) {
            static const char msg[] =
                "  [THREAD] a suspended thread waited a quarter of a second "
                "for a resume that did not come, and has carried on\n";
            ssize_t ignored = write(2, msg, sizeof msg - 1);
            (void)ignored;
            break;
        }
    }
    o->parked = 0;
}

static void w32_suspend_signal_install(void)
{
    static int done;
    struct sigaction sa;
    if (done) return;
    done = 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = w32_suspend_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(W32_SUSPEND_SIGNAL, &sa, NULL);
}

int w32_suspend_pending(void)
{
    const w32_object *o = t_self_obj;
    return o && o->suspend_count > 0 && !o->parked;
}

void w32_suspend_checkpoint(void)
{
    w32_object *o = t_self_obj;
    if (!o) return;
    pthread_mutex_lock(&o->lock);
    if (o->suspend_count > 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [THREAD] a thread suspended by another thread "
                            "has stopped at a checkpoint (this used not to "
                            "stop at all)\n");
        o->parked = 1;
        o->park_since = (long long)GetTickCount64();
        while (o->suspend_count > 0)
            pthread_cond_wait(&o->gate, &o->lock);
        o->parked = 0;
        pthread_cond_broadcast(&o->left_gate);
    }
    pthread_mutex_unlock(&o->lock);
}

DWORD SuspendThread(HANDLE h)
{
    /* Suspending ANOTHER thread mid-run is not supported on POSIX; only the
     * CREATE_SUSPENDED start gate is, so for those the count is tracked and
     * nothing stops. Suspending the CALLING thread is supported properly:
     * it blocks on the same gate until a ResumeThread brings the count back
     * to zero. Titles use SuspendThread(GetCurrentThread()) + ResumeThread
     * from another thread as a hand-off primitive (JSRF's loader threads
     * do), and a non-blocking version turns that into a busy spin. */
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? self_obj() : obj_check(h, K_THREAD);
    if (!o) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->pending_resume && o == self_obj()) {
        /* A resume got here first: consume it and carry on without parking.
         *
         * Only for a thread parking ITSELF. That is the race this exists for
         * -- the console's single CPU orders "give the worker work, resume it"
         * against "worker finishes and parks" in a way real cores do not. A
         * SuspendThread aimed at another thread is not that race; it is an
         * instruction to stop, and consuming a bank instead would mean the
         * target never stops at all. */
        static int said;
        o->pending_resume--;
        if (!said++)
            fprintf(stderr, "  [THREAD] a resume arrived before its suspend "
                            "and was held; the worker did not park\n");
        pthread_mutex_unlock(&o->lock);
        handoff_note('r', o->tid, 0);
        return prev;
    }
    o->suspend_count++;
    if (o == self_obj()) {
        o->parked = 1;
        o->park_since = (long long)GetTickCount64();
        pthread_mutex_unlock(&o->lock);
        handoff_note('S', o->tid, 1);
        pthread_mutex_lock(&o->lock);
        /*
         * Parked, but not forever.
         *
         * A title that hands work between threads with SuspendThread(self) +
         * ResumeThread(worker) is relying on a single CPU to order the two,
         * and here they race: measured on this title, about one suspend in
         * seven waits for a resume that never arrives. An untimed wait turns
         * that into a thread stopped for the rest of the run -- and a
         * sampling profile of a session that had stopped responding found
         * exactly that, one worker parked here for the whole twelve seconds
         * it was watched.
         *
         * Releasing it after a while is the lesser wrong. A thread that
         * carries on a quarter-second late is a hitch; a thread that never
         * carries on is the game. The count says how often it happens, so
         * this cannot quietly become normal.
         */
        {
            long long park0 = (long long)GetTickCount64();
            int timed_out = 0;
            while (o->suspend_count > 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50 * 1000 * 1000;      /* 50 ms */
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&o->gate, &o->lock, &ts);
                if ((long long)GetTickCount64() - park0 >= 400) { timed_out = 1; break; }
            }
            if (timed_out) {
                static unsigned long n;
                o->suspend_count = 0;
                if (++n <= 8 || (n % 256) == 0)
                    fprintf(stderr, "  [THREAD] thread %u waited %lld ms for a "
                            "resume that did not come and was released "
                            "(%lu so far)\n",
                            (unsigned)o->tid,
                            (long long)GetTickCount64() - park0, n);
            }
        }
        o->parked = 0;
        pthread_cond_broadcast(&o->left_gate);
        pthread_mutex_unlock(&o->lock);
        return prev;
    }
    pthread_mutex_unlock(&o->lock);
    /*
     * RECOMP_SUSPEND_REAL=1 makes a suspend aimed at another thread actually
     * stop it, by signalling the target wherever it is. Off by default, and
     * that is a retreat from a change made earlier today.
     *
     * The gap is real: POSIX cannot stop a running thread from outside, so
     * this layer has always tracked the count and let the target carry on, and
     * a title that suspends a worker to keep it away from shared state does
     * not get what it asked for. Making it real seemed clearly better.
     *
     * Measured, it is not. Across a forty-five second run the target waited
     * for a resume that never arrived thirty-eight times -- fourteen per cent
     * of all suspends -- and each of those is a thread stopped dead until a
     * timeout releases it. One run in four hung outright before the timeout
     * existed. Whether those resumes are lost by the title or by this
     * emulation is not yet known, and until it is, a thread that keeps running
     * when it should have stopped is a smaller and better-understood wrong
     * than a thread that stops and never starts.
     *
     * The evidence for the crash fix that landed alongside this points at the
     * banked-resume counter, not at this. So this stays, behind a switch, for
     * when the lost resumes are understood.
     */
    { static int real = -1;
      if (real < 0) real = getenv("RECOMP_SUSPEND_REAL") ? 1 : 0;
      if (real) {
          w32_suspend_signal_install();
          if (o->thread) pthread_kill(o->thread, W32_SUSPEND_SIGNAL);
      } }
    handoff_note('s', o->tid, (int)prev + 1);
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    w32_object *o = obj_any(h);
    if (!o || o->kind != K_THREAD) return FALSE;
    pthread_cancel(o->thread);
    pthread_mutex_lock(&o->lock);
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_check(h, K_THREAD);
    if (o && o->kind == K_THREAD) o->priority = priority;
    return TRUE;   /* real RT priorities need privileges; tracked only */
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_check(h, K_THREAD);
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

VOID SwitchToThread(void) { sched_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_check(thread, K_THREAD);
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
        o->apc_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

VOID Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    Sleep(ms);
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return 0;
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    int once = (o->timer_period == 0);

    /* initial due time */
    if (o->timer_due) Sleep(o->timer_due);
    if (!o->timer_cancel && o->timer_cb)
        o->timer_cb(o->timer_param, TRUE);

    while (!once && !o->timer_cancel) {
        Sleep(o->timer_period);
        if (o->timer_cancel) break;
        if (o->timer_cb) o->timer_cb(o->timer_param, TRUE);
    }
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return (HANDLE)obj_alloc(K_TIMER);
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    if (pthread_create(&o->thread, NULL, timer_thread, o) != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(o->thread);
    if (newTimer) *newTimer = (HANDLE)o;
    return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_due    = dueTime;
    o->timer_period = period;
    return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_cancel = 1;
    if (completionEvent) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    pthread_t th;
    if (pthread_create(&th, NULL, w32_tp_trampoline, a) != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

static int prot_from_page(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_READ | PROT_WRITE;
    }
}

/* ---- the guest address reservation -------------------------------------- */

/*
 * A range this process owns and can hand out at exact addresses.
 *
 * Placing a mapping by hint and checking where it landed is the right
 * behaviour when the address might belong to someone else -- it is what stops
 * a probe from destroying the C library. But the guest address space is not
 * someone else's: the console's RAM mirrors, its tiled aperture at
 * 0xF0000000 and its device windows all have to sit at exact offsets from the
 * base, and asking the kernel nicely for each one in turn means any of them
 * can be refused for reasons that have nothing to do with the emulation. On
 * macOS the tiled aperture was refused every run, and every render target
 * write then faulted.
 *
 * So the layout reserves the whole four gigabytes up front as unreadable
 * pages, and mappings that land inside it may use MAP_FIXED: replacing our
 * own reservation is exactly what it is for. Outside it, nothing changes.
 */
#define W32_MAX_RESERVATIONS 8
static struct { void *base; size_t size; } g_reserved[W32_MAX_RESERVATIONS];
static int g_reserved_n;

static int inside_reservation(const void *addr, size_t len)
{
    uintptr_t a = (uintptr_t)addr;
    int i;
    if (!addr) return 0;
    for (i = 0; i < g_reserved_n; i++) {
        uintptr_t b = (uintptr_t)g_reserved[i].base;
        if (a >= b && a + len > a && a + len <= b + g_reserved[i].size)
            return 1;
    }
    return 0;
}

void *win32_reserve_address_space(void *base, size_t size)
{
    void *p;
    if (g_reserved_n >= W32_MAX_RESERVATIONS) return NULL;
    p = mmap(base, size, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (base && p != base) { munmap(p, size); return NULL; }
    g_reserved[g_reserved_n].base = p;
    g_reserved[g_reserved_n].size = size;
    g_reserved_n++;
    return p;
}

/* ---- host page granularity ----------------------------------------------
 *
 * The console's pages are 4 KB and so are Windows'. Apple Silicon's are 16 KB,
 * and mprotect rounds outward: a guest asking to protect one 4 KB page there
 * changes the protection of the 16 KB around it, including up to three pages
 * that belong to something else. JSRF makes exactly that call and then faults
 * on a neighbouring heap page a moment later -- as SIGBUS rather than SIGSEGV,
 * because Darwin reports a protection failure that way, which is why it did
 * not look like a protection problem at all.
 *
 * A protection finer than the host can express is therefore not applied. That
 * loses a fault the guest was expecting to take; applying it anyway loses
 * memory the guest was still using, and only one of those two is recoverable.
 */
static size_t w32_host_page_size(void)
{
    static size_t ps;
    if (!ps) {
        long v = sysconf(_SC_PAGESIZE);
        ps = v > 0 ? (size_t)v : 4096;
    }
    return ps;
}

/* Apply a protection at the host's granularity, rounding in the direction
 * that cannot lose anything.
 *
 * Which direction that is depends on what the protection does. Granting
 * access -- a guest committing a page it is about to use -- is safe to round
 * outward: the neighbours become readable or writable when they did not need
 * to be, which costs a fault the guest was not going to take anyway. Removing
 * access is not: rounding outward there makes up to three pages belonging to
 * something else unwritable, and the something else then dies on an ordinary
 * store to memory it legitimately owns.
 *
 * Both halves of that were observed here, one after the other. Rounding
 * outward for everything made JSRF fault on a byte store 22 MB into its heap.
 * Rounding inward for everything left pages the guest had just committed
 * still unmapped, and it hung waiting on memory it thought it had. The rule
 * has to depend on the direction of the change, which is what this does. */
static int protect_within_host_pages(void *addr, size_t size, int prot)
{
    size_t ps = w32_host_page_size();
    uintptr_t a = (uintptr_t)addr, lo, hi;
    int grants = (prot & (PROT_READ | PROT_WRITE)) != 0;

    if (grants) {
        lo = a & ~(uintptr_t)(ps - 1);
        hi = (a + size + ps - 1) & ~(uintptr_t)(ps - 1);
    } else {
        lo = (a + ps - 1) & ~(uintptr_t)(ps - 1);
        hi = (a + size) & ~(uintptr_t)(ps - 1);
        if (hi <= lo) return 0;    /* finer than the host can express: skip */
    }
    return mprotect((void *)lo, (size_t)(hi - lo), prot);
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    /* MEM_COMMIT on a region already reserved by a prior VirtualAlloc:
     * just adjust protection. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address) {
        if (protect_within_host_pages(address, size, prot) == 0)
            return address;
        /* fall through to a fresh mapping */
    }

    int owned = inside_reservation(address, size);
    if (owned) flags |= MAP_FIXED;
#if defined(MAP_FIXED_NOREPLACE)
    else if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
    void *p = mmap(address, size, prot ? prot : PROT_READ | PROT_WRITE,
                   flags, -1, 0);
    if (p == MAP_FAILED) { SetLastError(8); return NULL; }
#if !defined(MAP_FIXED_NOREPLACE)
    if (owned) return p;
    /* Without MAP_FIXED_NOREPLACE -- Darwin, and older Linux -- a requested
     * address is only a hint, and the kernel is free to place the mapping
     * somewhere else entirely. MAP_FIXED is not the answer: it would take the
     * address by unmapping whatever already lives there, which for a caller
     * probing a list of candidate bases means silently destroying the mapping
     * it was about to reject. Placing it and checking gives Linux's semantics
     * exactly: the address, or failure. */
    if (address && p != address) {
        munmap(p, size);
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType & MEM_RELEASE) {
        /* Win32 MEM_RELEASE passes size 0; we can't know the length, so this
         * path is only safe when callers pass the real size. */
        if (size == 0) return TRUE;
        return munmap(address, size) == 0;
    }
    if (freeType & MEM_DECOMMIT) {
        if (getenv("JSRF_DBG")) fprintf(stderr, "  [VM] DECOMMIT %p size 0x%zx\n", address, size);
        return protect_within_host_pages(address, size, PROT_NONE) == 0;
    }
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    if (oldProtect) *oldProtect = PAGE_READWRITE;
    if (getenv("JSRF_DBG")) fprintf(stderr, "  [VM] PROTECT %p size 0x%zx -> 0x%x\n", address, size, (unsigned)newProtect);
    return protect_within_host_pages(address, size,
                                     prot_from_page(newProtect)) == 0;
}

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* The documented Darwin test: ask the kernel for this process's own proc
     * entry and look at P_TRACED. */
    {
        struct kinfo_proc info;
        size_t len = sizeof info;
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, 0 };
        mib[3] = getpid();
        memset(&info, 0, sizeof info);
        if (sysctl(mib, 4, &info, &len, NULL, 0) != 0) return FALSE;
        return (info.kp_proc.p_flag & P_TRACED) ? TRUE : FALSE;
    }
#else
    /* TODO: On Linux a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    return FALSE;
#endif
}

VOID DebugBreak(void) {
    // TODO: Use __debugbreak()?
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    /* Darwin's memset_s is only declared when __STDC_WANT_LIB_EXT1__ is set
     * before every string.h in the translation unit, which is not something a
     * compat header can promise. A volatile store cannot be optimised away
     * either, and needs no cooperation from libc. */
    {
        volatile unsigned char *q = (volatile unsigned char *)ptr;
        while (cnt--) *q++ = 0;
    }
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)share; (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, rw | extra, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return w32_open_handle(fd, name);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0)  { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nread)  *nread = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0)  { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nwritten) *nwritten = (DWORD)n;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Real keyboard polling will come   */
/* via SDL_GetKeyboardState when main.c gets its SDL2 port.              */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepad is wired through input_compat (SDL2). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed) -- true aliased mirror views              */
/* ===================================================================== */

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

static void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

static size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
}

/* An unnamed file descriptor that ftruncate and mmap both accept. Linux has
 * memfd_create for this; elsewhere an immediately-unlinked temp file does. */
static int anon_map_fd(const char *name)
{
#if defined(__APPLE__)
    /* Darwin has no memfd_create. POSIX shared memory is the closest thing:
     * shm_open gives a descriptor that ftruncate and MAP_SHARED both accept,
     * and unlinking the name immediately leaves an object that lives only as
     * long as the descriptor -- which is the whole point of memfd_create.
     *
     * The name must be unique and under SHM_NAME_MAX, and it must not survive
     * a crash, or the next run inherits a stale object of the wrong size.
     * Hence pid and a counter, and the unlink before anything else happens.
     *
     * shm_open is unavailable to sandboxed iOS apps, so fall back to a temp
     * file in whatever directory the process is allowed to write to. Unlinked
     * at once for the same reason. */
    {
        static int seq;
        char nm[64];
        int fd;
        snprintf(nm, sizeof nm, "/xbr.%d.%d", (int)getpid(), seq++);
        fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { shm_unlink(nm); return fd; }

        {
            const char *tmp = getenv("TMPDIR");
            char path[512];
            snprintf(path, sizeof path, "%s%sxbr.%d.%d.XXXXXX",
                     tmp ? tmp : "/tmp",
                     (tmp && tmp[0] && tmp[strlen(tmp) - 1] == '/') ? "" : "/",
                     (int)getpid(), seq++);
            fd = mkstemp(path);
            if (fd >= 0) unlink(path);
            return fd;
        }
    }
#else
    return memfd_create(name ? name : "xbox_map", 0);
#endif
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)file; (void)sa; (void)protect;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    o->fd       = fd;
    o->map_size = size;
    return (HANDLE)o;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = (w32_object *)mapping;
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    off_t  off = ((off_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PROT_READ | ((access != FILE_MAP_READ) ? PROT_WRITE : 0);

    /* A requested address is passed as a hint, not with MAP_FIXED.
     *
     * MAP_FIXED takes the range by unmapping whatever is already there, and
     * both callers of this function ask for an address they do not yet own:
     * the base view walks a list of candidate addresses expecting failure to
     * mean "taken", and the RAM mirrors ask for the aliases above it. Under
     * MAP_FIXED the walk cannot fail, so it stops at its first candidate
     * having destroyed whatever occupied it -- on a host where that address
     * is the C library or the thread stack, the crash arrives much later and
     * looks like anything but a mapping bug.
     *
     * A hint plus a check gives what the callers actually mean: the kernel
     * honours it when the range is free, and anything else is a refusal. */
    int owned = inside_reservation(baseAddr, len);
    void *p = mmap(baseAddr, len, prot,
                   MAP_SHARED | (owned ? MAP_FIXED : 0), o->fd, off);
    if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (!owned && baseAddr && p != baseAddr) {
        munmap(p, len);
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    view_register(p, len);
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (len == 0) return FALSE;
    return munmap((void *)baseAddr, len) == 0;
}

/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));
    buffer->BaseAddress    = (PVOID)address;
    buffer->AllocationBase = NULL;       /* != address -> freed via _aligned_free */
    buffer->RegionSize     = 0x1000;
    buffer->State          = MEM_COMMIT;
    buffer->Protect        = PAGE_READWRITE;
    buffer->Type           = 0x20000;    /* MEM_PRIVATE */
    return sizeof(*buffer);
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
#if defined(__APPLE__)
    /* Darwin has no sysinfo(2). Physical memory is a sysctl; the free page
     * count comes from the Mach VM statistics, where "available" has to
     * include the inactive and purgeable pages -- counting only the free list
     * reports a few hundred megabytes on a machine with tens of gigabytes
     * spare, because Darwin keeps almost everything in the file cache. */
    {
        uint64_t memsize = 0;
        size_t len = sizeof memsize;
        vm_statistics64_data_t vm;
        mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
        vm_size_t page = 0;

        if (!b) return FALSE;
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0) return FALSE;
        b->ullTotalPhys = memsize;
        b->ullAvailPhys = memsize;

        if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS
            && host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                 (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
            b->ullAvailPhys = (ULONGLONG)(vm.free_count + vm.inactive_count
                                          + vm.purgeable_count) * page;
        }
        /* No swap figure: vm.swapusage is a sysctl of its own and nothing
         * here needs it. Reporting page file as physical is honest enough for
         * a caller asking "how much room is there". */
        b->ullTotalPageFile = b->ullTotalPhys;
        b->ullAvailPageFile = b->ullAvailPhys;
    }
#else
    struct sysinfo si;
    if (!b) return FALSE;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* TODO: wire to sigaction */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

#endif /* !_WIN32 */

/* ---- SRWLOCK / INIT_ONCE (added for the POSIX build) ------------------- */
static pthread_mutex_t g_srw_boot = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t *srw_get(PSRWLOCK l)
{
    if (!l->Ptr) {
        pthread_mutex_lock(&g_srw_boot);
        if (!l->Ptr) {
            pthread_rwlock_t *rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            l->Ptr = rw;
        }
        pthread_mutex_unlock(&g_srw_boot);
    }
    return (pthread_rwlock_t *)l->Ptr;
}
VOID AcquireSRWLockShared(PSRWLOCK l)    { pthread_rwlock_rdlock(srw_get(l)); }
VOID ReleaseSRWLockShared(PSRWLOCK l)    { pthread_rwlock_unlock(srw_get(l)); }
VOID AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(srw_get(l)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(srw_get(l)); }
BOOL InitOnceExecuteOnce(PINIT_ONCE o, PINIT_ONCE_FN fn, PVOID param, PVOID *ctx)
{
    pthread_mutex_lock(&g_srw_boot);
    if (!o->Ptr) { fn(o, param, ctx); o->Ptr = (PVOID)1; }
    pthread_mutex_unlock(&g_srw_boot);
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    DWORD hi = 0, lo = GetFileSize(h, &hi);
    if (lo == 0xFFFFFFFFu && GetLastError() != 0) return FALSE;
    size->QuadPart = ((LONGLONG)hi << 32) | lo;
    return TRUE;
}
