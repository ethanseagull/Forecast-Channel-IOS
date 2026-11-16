// Port log: stderr plus <data dir>/boot.log, flushed per line so it survives
// a crash.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>

#include <port_log.h>
#include <port_paths.h>

static FILE* sLog;
static SDL_Mutex* sLock;

void port_log_open(void) {
    char path[1024];
    sLock = SDL_CreateMutex();
    SDL_snprintf(path, sizeof(path), "%sboot.log", port_data_dir());
    sLog = fopen(path, "w");
}

void port_log_v(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    if (sLock != NULL) {
        SDL_LockMutex(sLock);
    }
    vfprintf(stderr, fmt, ap);
    if (sLog != NULL) {
        vfprintf(sLog, fmt, ap2);
        fflush(sLog);
    }
    if (sLock != NULL) {
        SDL_UnlockMutex(sLock);
    }
    va_end(ap2);
}

void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    port_log_v(fmt, ap);
    va_end(ap);
}
