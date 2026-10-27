/* The POSIX subset MAG's opt-in diagnostics, and a few of the runtime's, use.
 *
 * Windows only: CMake puts src/win32/posix on the include path (and force-
 * includes this header) for MAG's sources and the runtime files that need it,
 * so <pthread.h>, <unistd.h> and <sched.h> resolve to the shims beside this
 * file. It supplies the CRT's POSIX-named descriptor calls, access/getpid,
 * clock_gettime (monotonic and thread CPU), nanosleep, sched_yield and a
 * pthread once/mutex/thread/self subset. Nothing here is on a hot path.
 * Diagnostic control-file paths are POSIX-absolute by contract, so those
 * diagnostics compile on Windows but cannot be armed there.
 */
#ifndef MAG_WIN32_POSIX_H
#define MAG_WIN32_POSIX_H
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MAG_WIN32_SSIZE_T
#define MAG_WIN32_SSIZE_T
typedef intptr_t ssize_t;
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096   /* buffer bound only */
#endif
#ifndef F_OK
#define F_OK 0
#endif
/* Callers open small control files read-only: binary mode keeps the bytes
 * exact, and _O_NOINHERIT is the CRT's close-on-exec. */
#ifndef O_CLOEXEC
#define O_CLOEXEC (_O_NOINHERIT | _O_BINARY)
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

typedef int clockid_t;
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

static inline int clock_gettime(clockid_t id, struct timespec* out)
{
    uint64_t ns;
    if (id == CLOCK_THREAD_CPUTIME_ID) {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
        ns = ((((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
              (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) * 100u;
    } else if (id == CLOCK_MONOTONIC) {
        LARGE_INTEGER counter, frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        ns = (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000000ull +
             (uint64_t)(counter.QuadPart % frequency.QuadPart) * 1000000000ull / (uint64_t)frequency.QuadPart;
    } else {
        return -1;
    }
    out->tv_sec = (time_t)(ns / 1000000000ull);
    out->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

static inline int nanosleep(const struct timespec* delay, struct timespec* remaining)
{
    (void)remaining;
    Sleep((DWORD)(delay->tv_sec * 1000 + (delay->tv_nsec + 999999) / 1000000));
    return 0;
}

static inline int sched_yield(void) { SwitchToThread(); return 0; }

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
static inline BOOL CALLBACK mag_win32_once_thunk(PINIT_ONCE once, PVOID routine, PVOID* context)
{
    (void)once; (void)context;
    ((void (*)(void))routine)();
    return TRUE;
}
static inline int pthread_once(pthread_once_t* once, void (*routine)(void))
{
    return InitOnceExecuteOnce(once, mag_win32_once_thunk, (PVOID)routine, NULL) ? 0 : -1;
}

typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static inline int pthread_mutex_lock(pthread_mutex_t* mutex) { AcquireSRWLockExclusive(mutex); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t* mutex) { ReleaseSRWLockExclusive(mutex); return 0; }

typedef HANDLE pthread_t;
typedef struct { void* (*routine)(void*); void* argument; } mag_win32_thread_start;
static inline unsigned __stdcall mag_win32_thread_thunk(void* opaque)
{
    mag_win32_thread_start start = *(mag_win32_thread_start*)opaque;
    free(opaque);
    start.routine(start.argument);
    return 0;
}
static inline int pthread_create(pthread_t* thread, const void* attributes, void* (*routine)(void*), void* argument)
{
    (void)attributes;
    mag_win32_thread_start* start = (mag_win32_thread_start*)malloc(sizeof *start);
    if (!start) return -1;
    start->routine = routine; start->argument = argument;
    uintptr_t handle = _beginthreadex(NULL, 0, mag_win32_thread_thunk, start, 0, NULL);
    if (!handle) { free(start); return -1; }
    *thread = (HANDLE)handle;
    return 0;
}
static inline int pthread_detach(pthread_t thread) { return CloseHandle(thread) ? 0 : -1; }
static inline int pthread_join(pthread_t thread, void** result)
{
    if (result) *result = NULL;
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) return -1;
    CloseHandle(thread);
    return 0;
}
/* Identities only: callers record them, never join them. */
static inline pthread_t pthread_self(void) { return (pthread_t)(uintptr_t)GetCurrentThreadId(); }
static inline int pthread_threadid_np(void* thread, uint64_t* id) { (void)thread; *id = GetCurrentThreadId(); return 0; }

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* MAG_WIN32_POSIX_H */
