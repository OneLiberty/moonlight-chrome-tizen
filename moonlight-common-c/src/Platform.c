#define _GNU_SOURCE
#include "Limelight-internal.h"

// The maximum amount of time before observing an interrupt
// in PltSleepMsInterruptible().
#define INTERRUPT_PERIOD_MS 50

struct thread_context {
    ThreadEntry entry;
    void* context;
    const char* name;
#if defined(__vita__)
    PLT_THREAD* thread;
#endif
};

static int activeThreads = 0;
static int activeMutexes = 0;
static int activeEvents = 0;
static int activeCondVars = 0;

#if defined(LC_WINDOWS)

#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO
{
    DWORD dwType; // Must be 0x1000.
    LPCSTR szName; // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

typedef HRESULT (WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);

void setThreadNameWin32(const char* name) {
    SetThreadDescription_t setThreadDescriptionFunc;

    // This function is only supported on Windows 10 RS1 and later
    setThreadDescriptionFunc = (SetThreadDescription_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (setThreadDescriptionFunc != NULL) {
        WCHAR nameW[16];
        size_t chars;

        mbstowcs_s(&chars, nameW, ARRAYSIZE(nameW), name, _TRUNCATE);
        setThreadDescriptionFunc(GetCurrentThread(), nameW);
    }

#ifdef _MSC_VER
    // This method works on legacy OSes and older tools not updated to use SetThreadDescription yet,
    // but it's only safe on MSVC with SEH
    if (IsDebuggerPresent()) {
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;
        info.dwThreadID = (DWORD)-1;
        info.dwFlags = 0;
        __try {
            RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Nothing
        }
    }
#endif
}

DWORD WINAPI ThreadProc(LPVOID lpParameter) {
    struct thread_context* ctx = (struct thread_context*)lpParameter;
#elif defined(__vita__)
int ThreadProc(SceSize args, void *argp) {
    struct thread_context* ctx = (struct thread_context*)argp;
#elif defined(__WIIU__)
int ThreadProc(int argc, const char** argv) {
    struct thread_context* ctx = (struct thread_context*)argv;
#else
void* ThreadProc(void* context) {
    struct thread_context* ctx = (struct thread_context*)context;
#endif

#if defined(LC_WINDOWS)
    setThreadNameWin32(ctx->name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), ctx->name);
#elif defined(LC_DARWIN)
    pthread_setname_np(ctx->name);
#endif

    ctx->entry(ctx->context);

#if defined(__vita__)
    if (ctx->thread->detached) {
        free(ctx);
        sceKernelExitDeleteThread(0);
    }
    else {
        free(ctx);
    }
#else
    free(ctx);
#endif

#if defined(LC_WINDOWS) || defined(__vita__) || defined(__WIIU__) || defined(__3DS__)
    return 0;
#else
    return NULL;
#endif
}

void PltSleepMs(int ms) {
#if defined(LC_WINDOWS)
    SleepEx(ms, FALSE);
#elif defined(__vita__)
    sceKernelDelayThread(ms * 1000);
#elif defined(__3DS__)
    s64 nsecs = ms * 1000000;
    svcSleepThread(nsecs);
#else
    useconds_t usecs = ms * 1000;
    usleep(usecs);
#endif
}

void PltSleepMsInterruptible(PLT_THREAD* thread, int ms) {
    while (ms > 0 && !PltIsThreadInterrupted(thread)) {
        int msToSleep = ms < INTERRUPT_PERIOD_MS ? ms : INTERRUPT_PERIOD_MS;
        PltSleepMs(msToSleep);
        ms -= msToSleep;
    }
}

int PltCreateMutex(PLT_MUTEX* mutex) {
#if defined(LC_WINDOWS)
    InitializeSRWLock(mutex);
#elif defined(__vita__)
    *mutex = sceKernelCreateMutex("", 0, 0, NULL);
    if (*mutex < 0) {
        return -1;
    }
#elif defined(__WIIU__)
    OSFastMutex_Init(mutex, "");
#elif defined(__3DS__)
    LightLock_Init(mutex);
#else
    int err = pthread_mutex_init(mutex, NULL);
    if (err != 0) {
        return err;
    }
#endif
    activeMutexes++;
    return 0;
}

void PltDeleteMutex(PLT_MUTEX* mutex) {
    LC_ASSERT(activeMutexes > 0);
    activeMutexes--;
#if defined(LC_WINDOWS)
    // No-op to destroy a SRWLOCK
#elif defined(__vita__)
    sceKernelDeleteMutex(*mutex);
#elif defined(__WIIU__) || defined(__3DS__)

#else
    pthread_mutex_destroy(mutex);
#endif
}

void PltLockMutex(PLT_MUTEX* mutex) {
#if defined(LC_WINDOWS)
    AcquireSRWLockExclusive(mutex);
#elif defined(__vita__)
    sceKernelLockMutex(*mutex, 1, NULL);
#elif defined(__WIIU__)
    OSFastMutex_Lock(mutex);
#elif defined(__3DS__)
    LightLock_Lock(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

void PltUnlockMutex(PLT_MUTEX* mutex) {
#if defined(LC_WINDOWS)
    ReleaseSRWLockExclusive(mutex);
#elif defined(__vita__)
    sceKernelUnlockMutex(*mutex, 1);
#elif defined(__WIIU__)
    OSFastMutex_Unlock(mutex);
#elif defined(__3DS__)
    LightLock_Unlock(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

void PltJoinThread(PLT_THREAD* thread) {
    LC_ASSERT(activeThreads > 0);
    activeThreads--;

#if defined(LC_WINDOWS)
    WaitForSingleObjectEx(thread->handle, INFINITE, FALSE);
    CloseHandle(thread->handle);
#elif defined(__vita__)
    LC_ASSERT(!thread->detached);
    sceKernelWaitThreadEnd(thread->handle, NULL, NULL);
    sceKernelDeleteThread(thread->handle);
#elif defined(__WIIU__)
    OSJoinThread(&thread->thread, NULL);
#elif defined(__3DS__)
    threadJoin(thread->thread, U64_MAX);
    threadFree(thread->thread);
#else
    pthread_join(thread->thread, NULL);
#endif
}

void PltDetachThread(PLT_THREAD* thread) {
    LC_ASSERT(activeThreads > 0);
    activeThreads--;

#if defined(LC_WINDOWS)
    // According MSDN:
    // "Closing a thread handle does not terminate the associated thread or remove the thread object."
    CloseHandle(thread->handle);
#elif defined(__vita__)
    LC_ASSERT(!thread->detached);
    thread->detached = true;
#elif defined(__WIIU__)
    OSDetachThread(&thread->thread);
#elif defined(__3DS__)
    threadDetach(thread->thread);
#else
    pthread_detach(thread->thread);
#endif
}

bool PltIsThreadInterrupted(PLT_THREAD* thread) {
    return thread->cancelled;
}

void PltInterruptThread(PLT_THREAD* thread) {
    thread->cancelled = true;
}

#ifdef __WIIU__
static void thread_deallocator(OSThread *thread, void *stack) {
    free(stack);
}
#endif

int PltCreateThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread) {
    struct thread_context* ctx;

    ctx = (struct thread_context*)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }

    ctx->entry = entry;
    ctx->context = context;
    ctx->name = name;

    thread->cancelled = false;

#if defined(LC_WINDOWS)
    {
        thread->handle = CreateThread(NULL, 0, ThreadProc, ctx, 0, NULL);
        if (thread->handle == NULL) {
            free(ctx);
            return -1;
        }
    }
#elif defined(__vita__)
    {
        thread->detached = false;
        thread->context = ctx;
        ctx->thread = thread;
        thread->handle = sceKernelCreateThread(name, ThreadProc, 0, 0x40000, 0, 0, NULL);
        if (thread->handle < 0) {
            free(ctx);
            return -1;
        }
        sceKernelStartThread(thread->handle, sizeof(struct thread_context), ctx);
    }
#elif defined(__WIIU__)
    memset(&thread->thread, 0, sizeof(thread->thread));

    // Allocate stack
    const int stack_size = 4 * 1024 * 1024;
    uint8_t* stack = (uint8_t*)memalign(16, stack_size);
    if (stack == NULL) {
        free(ctx);
        return -1;
    }

    // Create thread
    if (!OSCreateThread(&thread->thread,
                        ThreadProc,
                        0, (char*)ctx,
                        stack + stack_size, stack_size,
                        0x10, OS_THREAD_ATTRIB_AFFINITY_ANY))
    {
        free(ctx);
        free(stack);
        return -1;
    }

    OSSetThreadName(&thread->thread, name);
    OSSetThreadDeallocator(&thread->thread, thread_deallocator);
    OSResumeThread(&thread->thread);
#elif defined(__3DS__)
    {
        size_t stack_size = 0x40000;
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        thread->thread = threadCreate(ThreadProc,
                                    ctx,
                                    stack_size,
                                    priority,
                                    -1,
                                    false);
        if (thread->thread == NULL) {
            free(ctx);
            return -1;
        }
    }
#else
    {
        int err = pthread_create(&thread->thread, NULL, ThreadProc, ctx);
        if (err != 0) {
            free(ctx);
            return err;
        }
    }
#endif

    activeThreads++;

    return 0;
}

int PltCreateEvent(PLT_EVENT* event) {
#if defined(LC_WINDOWS)
    *event = CreateEventEx(NULL, NULL, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    if (!*event) {
        return -1;
    }
#else
    if (PltCreateMutex(&event->mutex) < 0) {
        return -1;
    }
    if (PltCreateConditionVariable(&event->cond, &event->mutex) < 0) {
        PltDeleteMutex(&event->mutex);
        return -1;
    }
    event->signalled = false;
#endif
    activeEvents++;
    return 0;
}

void PltCloseEvent(PLT_EVENT* event) {
    LC_ASSERT(activeEvents > 0);
    activeEvents--;
#if defined(LC_WINDOWS)
    CloseHandle(*event);
#else
    PltDeleteConditionVariable(&event->cond);
    PltDeleteMutex(&event->mutex);
#endif
}

void PltSetEvent(PLT_EVENT* event) {
#if defined(LC_WINDOWS)
    SetEvent(*event);
#else
    PltLockMutex(&event->mutex);
    event->signalled = true;
    PltUnlockMutex(&event->mutex);
    PltSignalConditionVariable(&event->cond);
#endif
}

void PltClearEvent(PLT_EVENT* event) {
#if defined(LC_WINDOWS)
    ResetEvent(*event);
#else
    event->signalled = false;
#endif
}

void PltWaitForEvent(PLT_EVENT* event) {
#if defined(LC_WINDOWS)
    WaitForSingleObjectEx(*event, INFINITE, FALSE);
#else
    PltLockMutex(&event->mutex);
    while (!event->signalled) {
        PltWaitForConditionVariable(&event->cond, &event->mutex);
    }
    PltUnlockMutex(&event->mutex);
#endif
}

int PltCreateConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
#if defined(LC_WINDOWS)
    InitializeConditionVariable(cond);
#elif defined(__vita__)
    *cond = sceKernelCreateCond("", 0, *mutex, NULL);
    if (*cond < 0) {
        return -1;
    }
#elif defined(__WIIU__)
    OSFastCond_Init(cond, "");
#elif defined(__3DS__)
    CondVar_Init(cond);
#else
    pthread_cond_init(cond, NULL);
#endif
    activeCondVars++;
    return 0;
}

void PltDeleteConditionVariable(PLT_COND* cond) {
    LC_ASSERT(activeCondVars > 0);
    activeCondVars--;
#if defined(LC_WINDOWS)
    // No-op to delete a CONDITION_VARIABLE
#elif defined(__vita__)
    sceKernelDeleteCond(*cond);
#elif defined(__WIIU__)
    // No-op to delete an OSFastCondition
#elif defined(__3DS__)
    // No-op to delete CondVar
#else
    pthread_cond_destroy(cond);
#endif
}

void PltSignalConditionVariable(PLT_COND* cond) {
#if defined(LC_WINDOWS)
    WakeConditionVariable(cond);
#elif defined(__vita__)
    sceKernelSignalCond(*cond);
#elif defined(__WIIU__)
    OSFastCond_Signal(cond);
#elif defined(__3DS__)
    CondVar_Signal(cond);
#else
    pthread_cond_signal(cond);
#endif
}

void PltWaitForConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
#if defined(LC_WINDOWS)
    SleepConditionVariableSRW(cond, mutex, INFINITE, 0);
#elif defined(__vita__)
    sceKernelWaitCond(*cond, NULL);
#elif defined(__WIIU__)
    OSFastCond_Wait(cond, mutex);
#elif defined(__3DS__)
    CondVar_Wait(cond, mutex);
#else
    pthread_cond_wait(cond, mutex);
#endif
}

uint64_t PltGetMillis(void) {
#if defined(LC_WINDOWS)
    return GetTickCount64();
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
    struct timespec tv;

    clock_gettime(CLOCK_MONOTONIC, &tv);

    return ((uint64_t)tv.tv_sec * 1000) + (tv.tv_nsec / 1000000);
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((uint64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
#endif
}

bool PltSafeStrcpy(char* dest, size_t dest_size, const char* src) {
    LC_ASSERT(dest_size > 0);

#ifdef LC_DEBUG
    // In debug builds, do the same little trick that MSVC
    // does to ensure the entire buffer is writable.
    memset(dest, 0xFE, dest_size);
#endif

#ifdef _MSC_VER
    // strncpy_s() with _TRUNCATE does what we need for MSVC.
    // We use this rather than strcpy_s() because we don't want
    // the invalid parameter handler invoked upon failure.
    if (strncpy_s(dest, dest_size, src, _TRUNCATE) != 0) {
        LC_ASSERT(false);
        dest[0] = 0;
        return false;
    }
#else
    // Check length of the source and destination strings before
    // the strcpy() call. Set destination to an empty string if
    // the source string doesn't fit in the destination.
    if (strlen(src) >= dest_size) {
        LC_ASSERT(false);
        dest[0] = 0;
        return false;
    }

    strcpy(dest, src);
#endif

    return true;
}

int initializePlatform(void) {
    int err;

    err = initializePlatformSockets();
    if (err != 0) {
        return err;
    }

    err = enet_initialize();
    if (err != 0) {
        return err;
    }

    enterLowLatencyMode();

    return 0;
}

void cleanupPlatform(void) {
    exitLowLatencyMode();

    cleanupPlatformSockets();

    enet_deinitialize();

    LC_ASSERT(activeThreads == 0);
    LC_ASSERT(activeMutexes == 0);
    LC_ASSERT(activeEvents == 0);
    LC_ASSERT(activeCondVars == 0);
}
