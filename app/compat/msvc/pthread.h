/*
 * Minimal pthread shim for building UxPlay's lib/ with MSVC.
 *
 * UxPlay's threads.h uses exactly: pthread_create, pthread_join, the mutex
 * init/lock/unlock/destroy calls, and cond init/signal/destroy/timedwait.
 * Only those are provided here, on top of Win32 threads, SRWLOCK and
 * CONDITION_VARIABLE.
 *
 * SRWLOCK rather than CRITICAL_SECTION on purpose: UxPlay locks at least one
 * mutex (httpd.c run_mutex) that it never initializes, relying on zeroed memory
 * being a valid unlocked pthread mutex. A zeroed SRWLOCK is valid too; a zeroed
 * CRITICAL_SECTION is not and faults in ntdll.
 */
#ifndef AIRPLAY_COMPAT_PTHREAD_H
#define AIRPLAY_COMPAT_PTHREAD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pthread_t must be a scalar: UxPlay assigns 0 to it and tests it. */
typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static __inline int clock_gettime(int clock_id, struct timespec *ts)
{
    /* 100 ns ticks between 1601-01-01 and 1970-01-01 */
    const uint64_t epoch_offset = 116444736000000000ULL;
    FILETIME ft;
    ULARGE_INTEGER ticks;
    (void) clock_id;
    GetSystemTimePreciseAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    ticks.QuadPart -= epoch_offset;
    ts->tv_sec = (time_t) (ticks.QuadPart / 10000000ULL);
    ts->tv_nsec = (long) ((ticks.QuadPart % 10000000ULL) * 100ULL);
    return 0;
}

typedef struct {
    void *(*start)(void *);
    void *arg;
} airplay_compat_thread_start_t;

static unsigned __stdcall airplay_compat_thread_entry(void *param)
{
    airplay_compat_thread_start_t start = *(airplay_compat_thread_start_t *) param;
    free(param);
    start.start(start.arg);
    return 0;
}

static __inline int pthread_create(pthread_t *thread, const void *attr,
                                   void *(*start)(void *), void *arg)
{
    airplay_compat_thread_start_t *param;
    uintptr_t handle;
    (void) attr;
    param = (airplay_compat_thread_start_t *) malloc(sizeof(*param));
    if (!param) {
        return ENOMEM;
    }
    param->start = start;
    param->arg = arg;
    handle = _beginthreadex(NULL, 0, airplay_compat_thread_entry, param, 0, NULL);
    if (!handle) {
        free(param);
        return EAGAIN;
    }
    *thread = (HANDLE) handle;
    return 0;
}

static __inline int pthread_join(pthread_t thread, void **retval)
{
    (void) retval;
    if (!thread) {
        return ESRCH;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static __inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void) attr;
    InitializeSRWLock(mutex);
    return 0;
}

static __inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static __inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static __inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void) mutex;
    return 0;
}

static __inline int pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    (void) attr;
    InitializeConditionVariable(cond);
    return 0;
}

static __inline int pthread_cond_signal(pthread_cond_t *cond)
{
    WakeConditionVariable(cond);
    return 0;
}

static __inline int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void) cond;
    return 0;
}

/* abstime is an absolute CLOCK_REALTIME deadline, as in POSIX. */
static __inline int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                           const struct timespec *abstime)
{
    struct timespec now;
    int64_t remaining_ms;
    clock_gettime(CLOCK_REALTIME, &now);
    remaining_ms = ((int64_t) abstime->tv_sec - (int64_t) now.tv_sec) * 1000
                 + ((int64_t) abstime->tv_nsec - (int64_t) now.tv_nsec) / 1000000;
    if (remaining_ms < 0) {
        remaining_ms = 0;
    }
    if (!SleepConditionVariableSRW(cond, mutex, (DWORD) remaining_ms, 0)) {
        return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* AIRPLAY_COMPAT_PTHREAD_H */
