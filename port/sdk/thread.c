// OS threads, mutexes, message queues and alarms on SDL3.
//
// Port state lives inside the SDK's own structures, so their sizes stay as the
// channel expects:
//  - OSThread: the register save area (OSContext, 0x2C8 bytes) holds a PortThread.
//  - OSMutex: its thread queue holds the SDL mutex (SDL mutexes recurse, like
//    the Wii's).
//  - OSMessageQueue: its two thread queues hold a mutex and two conditions.
// Alarm handlers run on one service thread while holding the "interrupts
// disabled" lock, which is what Wii code assumes about them.
#include <SDL3/SDL.h>
#include <stddef.h>
#include <string.h>

#include <revolution/OS.h>

#include <port_log.h>

typedef struct PortThread {
    SDL_Thread* handle;
    OSThreadFunc func;
    void* arg;
    void* result;
    u32 stackSize;
    s32 priority;
    u16 attr; // OSCreateThread flags: bit 0 = detached
    SDL_AtomicInt started;
    SDL_AtomicInt finished;
} PortThread;

_Static_assert(sizeof(PortThread) <= sizeof(OSContext), "PortThread must fit in OSContext");

static PortThread* port_thread(OSThread* t) { return (PortThread*)&t->context; }

static OSThread sMainThread;
static _Thread_local OSThread* sCurrentThread;
static _Thread_local int sHostThread, sGameThread;

void port_mark_host_thread(void) { sHostThread = 1; }
int port_is_host_thread(void) { return sHostThread; }
void port_mark_game_thread(void) { sGameThread = 1; }
int port_is_game_thread(void) { return sGameThread && !sHostThread; }

OSThread* OSGetCurrentThread(void) {
    return sCurrentThread != NULL ? sCurrentThread : &sMainThread;
}

// Wii priorities: 0 (highest) .. 31, main thread 16.
static SDL_ThreadPriority to_sdl_priority(s32 prio) {
    return prio < 12 ? SDL_THREAD_PRIORITY_HIGH : prio > 20 ? SDL_THREAD_PRIORITY_LOW : SDL_THREAD_PRIORITY_NORMAL;
}

static int thread_entry(void* arg) {
    OSThread* t = (OSThread*)arg;
    PortThread* pt = port_thread(t);
    sCurrentThread = t;
    port_mark_game_thread();
    SDL_SetCurrentThreadPriority(to_sdl_priority(pt->priority));
    pt->result = pt->func(pt->arg);
    SDL_SetAtomicInt(&pt->finished, 1);
    return 0;
}

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = NULL;
    queue->tail = NULL;
}

BOOL OSCreateThread(OSThread* thread, OSThreadFunc func, void* funcArg, void* stackBegin, u32 stackSize,
                    s32 prio, u16 flags) {
    (void)stackBegin; // SDL allocates its own stack
    memset(thread, 0, sizeof(*thread));
    PortThread* pt = port_thread(thread);
    pt->attr = flags;
    pt->func = func;
    pt->arg = funcArg;
    // Host code (C library, SDL) needs more stack than the Wii's.
    pt->stackSize = stackSize < 0x40000 ? 0x40000 : stackSize;
    pt->priority = prio;
    thread->priority = prio;
    thread->suspend = 1; // created suspended, like the Wii
    return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
    PortThread* pt = port_thread(thread);
    s32 prev = thread->suspend;
    if (thread->suspend > 0) {
        thread->suspend--;
    }
    if (thread->suspend == 0 && SDL_CompareAndSwapAtomicInt(&pt->started, 0, 1)) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void*)thread_entry);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "OSThread");
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, thread);
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, pt->stackSize);
        pt->handle = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
        if (pt->handle == NULL) {
            port_log("OSResumeThread: SDL_CreateThread failed: %s\n", SDL_GetError());
            SDL_SetAtomicInt(&pt->finished, 1);
        }
    }
    return prev;
}

s32 OSSuspendThread(OSThread* thread) {
    // Only meaningful before the thread starts; running host threads cannot
    // be suspended from outside.
    return thread->suspend++;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return SDL_GetAtomicInt(&port_thread(thread)->finished) != 0;
}

// The thread attribute OSCreateThread takes: a detached thread cannot be
// joined.
#define PORT_THREAD_ATTR_DETACH 1

BOOL OSJoinThread(OSThread* thread, void* val) {
    PortThread* pt = port_thread(thread);
    // As on the Wii, joining a detached thread fails at once instead of
    // waiting (WiiConnect24's downloader is detached and never exits; the
    // channel joins it when returning to the menu).
    if (pt->attr & PORT_THREAD_ATTR_DETACH) {
        return FALSE;
    }
    if (pt->handle != NULL) {
        SDL_WaitThread(pt->handle, NULL);
        pt->handle = NULL;
    }
    if (val != NULL) {
        *(void**)val = pt->result;
    }
    return TRUE;
}

void OSDetachThread(OSThread* thread) {
    PortThread* pt = port_thread(thread);
    pt->attr |= PORT_THREAD_ATTR_DETACH;
    if (pt->handle != NULL) {
        SDL_DetachThread(pt->handle);
        pt->handle = NULL;
    }
}

void OSExitThread(OSThread* thread) {
    port_log("OSExitThread: unsupported on the host (thread keeps running)\n");
    (void)thread;
}

void OSCancelThread(OSThread* thread) {
    port_log("OSCancelThread: unsupported, ignored\n");
    (void)thread;
}

void OSYieldThread(void) { SDL_DelayNS(0); }

void OSSleepTicks(s64 ticks) {
    // ticks are OS_TIME_SPEED units: 4000 / 243 ns each
    if (ticks > 0) {
        SDL_DelayPrecise((Uint64)ticks * 4000 / 243);
    }
}

// ---------------------------------------------------------------- mutexes

static SDL_Mutex* mutex_get(OSMutex* mutex) {
    SDL_Mutex* m = (SDL_Mutex*)mutex->queue.head;
    if (m == NULL) {
        // Never initialised (zeroed static): create it under the global lock.
        BOOL level = OSDisableInterrupts();
        m = (SDL_Mutex*)mutex->queue.head;
        if (m == NULL) {
            m = SDL_CreateMutex();
            mutex->queue.head = (OSThread*)m;
        }
        OSRestoreInterrupts(level);
    }
    return m;
}

void OSInitMutex(OSMutex* mutex) {
    memset(mutex, 0, sizeof(*mutex));
    mutex->queue.head = (OSThread*)SDL_CreateMutex();
}

void OSLockMutex(OSMutex* mutex) { SDL_LockMutex(mutex_get(mutex)); }

void OSUnlockMutex(OSMutex* mutex) { SDL_UnlockMutex(mutex_get(mutex)); }

BOOL OSTryLockMutex(OSMutex* mutex) { return SDL_TryLockMutex(mutex_get(mutex)); }

// ---------------------------------------------------------------- message queues

typedef struct PortQueueSync {
    SDL_Mutex* lock;
    SDL_Condition* notEmpty;
    SDL_Condition* notFull;
} PortQueueSync;

_Static_assert(sizeof(PortQueueSync) <= offsetof(OSMessageQueue, buffer), "queue sync must fit");

static PortQueueSync* queue_sync(OSMessageQueue* q) { return (PortQueueSync*)q; }

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* buffer, s32 capacity) {
    PortQueueSync* s = queue_sync(queue);
    s->lock = SDL_CreateMutex();
    s->notEmpty = SDL_CreateCondition();
    s->notFull = SDL_CreateCondition();
    queue->buffer = buffer;
    queue->capacity = capacity;
    queue->front = 0;
    queue->size = 0;
}

static BOOL queue_put(OSMessageQueue* queue, OSMessage mesg, u32 flags, BOOL jam) {
    PortQueueSync* s = queue_sync(queue);
    SDL_LockMutex(s->lock);
    while (queue->size >= queue->capacity) {
        if (!(flags & OS_MSG_BLOCKING)) {
            SDL_UnlockMutex(s->lock);
            return FALSE;
        }
        SDL_WaitCondition(s->notFull, s->lock);
    }
    if (jam) {
        queue->front = (queue->front + queue->capacity - 1) % queue->capacity;
        queue->buffer[queue->front] = mesg;
    } else {
        queue->buffer[(queue->front + queue->size) % queue->capacity] = mesg;
    }
    queue->size++;
    SDL_SignalCondition(s->notEmpty);
    SDL_UnlockMutex(s->lock);
    return TRUE;
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage mesg, u32 flags) { return queue_put(queue, mesg, flags, FALSE); }

BOOL OSJamMessage(OSMessageQueue* queue, OSMessage mesg, u32 flags) { return queue_put(queue, mesg, flags, TRUE); }

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* mesg, u32 flags) {
    PortQueueSync* s = queue_sync(queue);
    SDL_LockMutex(s->lock);
    while (queue->size == 0) {
        if (!(flags & OS_MSG_BLOCKING)) {
            SDL_UnlockMutex(s->lock);
            return FALSE;
        }
        SDL_WaitCondition(s->notEmpty, s->lock);
    }
    if (mesg != NULL) {
        *mesg = queue->buffer[queue->front];
    }
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size--;
    SDL_SignalCondition(s->notFull);
    SDL_UnlockMutex(s->lock);
    return TRUE;
}

// ---------------------------------------------------------------- alarms

#define PORT_MAX_ALARMS 32
static OSAlarm* sAlarms[PORT_MAX_ALARMS];
static SDL_Mutex* sAlarmLock;
static SDL_AtomicInt sAlarmInit;

static int alarm_thread(void* arg) {
    (void)arg;
    for (;;) {
        SDL_DelayNS(1000000); // 1 ms resolution
        s64 now = OSGetTime();
        for (int i = 0; i < PORT_MAX_ALARMS; i++) {
            SDL_LockMutex(sAlarmLock);
            OSAlarm* a = sAlarms[i];
            OSAlarmHandler handler = NULL;
            if (a != NULL && a->handler != NULL && now >= a->end) {
                handler = a->handler;
                if (a->period > 0) {
                    while (a->end <= now) {
                        a->end += a->period;
                    }
                } else {
                    sAlarms[i] = NULL;
                    a->handler = NULL;
                }
            }
            SDL_UnlockMutex(sAlarmLock);
            if (handler != NULL) {
                BOOL level = OSDisableInterrupts();
                handler(a, NULL);
                OSRestoreInterrupts(level);
            }
        }
    }
    return 0;
}

static void alarm_init(void) {
    if (SDL_CompareAndSwapAtomicInt(&sAlarmInit, 0, 1)) {
        sAlarmLock = SDL_CreateMutex();
        SDL_DetachThread(SDL_CreateThread(alarm_thread, "OSAlarm", NULL));
        SDL_SetAtomicInt(&sAlarmInit, 2);
    }
    while (SDL_GetAtomicInt(&sAlarmInit) != 2) {
        SDL_DelayNS(0);
    }
}

void OSCreateAlarm(OSAlarm* alarm) {
    alarm_init();
    alarm->handler = NULL;
    alarm->tag = 0;
}

static void alarm_insert(OSAlarm* alarm) {
    SDL_LockMutex(sAlarmLock);
    for (int i = 0; i < PORT_MAX_ALARMS; i++) {
        if (sAlarms[i] == alarm) {
            SDL_UnlockMutex(sAlarmLock);
            return;
        }
    }
    for (int i = 0; i < PORT_MAX_ALARMS; i++) {
        if (sAlarms[i] == NULL) {
            sAlarms[i] = alarm;
            SDL_UnlockMutex(sAlarmLock);
            return;
        }
    }
    SDL_UnlockMutex(sAlarmLock);
    port_log("OSSetAlarm: too many alarms\n");
}

void OSSetAlarm(OSAlarm* alarm, s64 tick, OSAlarmHandler handler) {
    alarm_init();
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = OSGetTime();
    alarm->end = alarm->start + tick;
    alarm_insert(alarm);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, s64 tick, s64 period, OSAlarmHandler handler) {
    alarm_init();
    alarm->handler = handler;
    alarm->period = period;
    alarm->start = tick;
    alarm->end = tick + period;
    alarm_insert(alarm);
}

void OSCancelAlarm(OSAlarm* alarm) {
    alarm_init();
    SDL_LockMutex(sAlarmLock);
    for (int i = 0; i < PORT_MAX_ALARMS; i++) {
        if (sAlarms[i] == alarm) {
            sAlarms[i] = NULL;
        }
    }
    alarm->handler = NULL;
    SDL_UnlockMutex(sAlarmLock);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }

void OSSetAlarmUserData(OSAlarm* alarm, void* userData) { alarm->userData = userData; }

void* OSGetAlarmUserData(const OSAlarm* alarm) { return alarm->userData; }
