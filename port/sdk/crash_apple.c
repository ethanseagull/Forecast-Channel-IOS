// Crash reports on Apple platforms (iOS): on a fatal signal, appends the
// signal, the faulting address, the registers that matter and a stack walk as
// offsets into the executable to boot.log in the data folder (the app's
// Documents, visible in the Files app), then lets the crash proceed (so iOS
// still writes its own report). Symbolize with
//   llvm-symbolizer --obj=ForecastOS --adjust-vma=0x100000000 0x<offset> ...
// (the Mach-O's __TEXT starts at 0x100000000 in the file's address space).
// Installed by a constructor, before the decomp's static initialisers.
#ifdef __APPLE__
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <port_paths.h>

static uintptr_t image_base(void) {
    return (uintptr_t)_dyld_get_image_header(0);
}

static void line(FILE* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (f != NULL) {
        va_list ap2;
        va_copy(ap2, ap);
        vfprintf(f, fmt, ap2);
        va_end(ap2);
    }
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void handler(int sig, siginfo_t* info, void* ctx) {
    char path[1100];
    const char* dir = port_data_dir();
    snprintf(path, sizeof(path), "%sboot.log", dir != NULL ? dir : "");
    FILE* f = dir != NULL && dir[0] != 0 ? fopen(path, "a") : NULL;
    uintptr_t base = image_base();

    line(f, "\n*** crash: signal %d (%s) at address %p\n", sig, strsignal(sig), info != NULL ? info->si_addr : NULL);
#if defined(__arm64__) || defined(__aarch64__)
    if (ctx != NULL) {
        ucontext_t* uc = (ucontext_t*)ctx;
        uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        uintptr_t lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
        line(f, "pc exe+0x%lx, lr exe+0x%lx\n", (unsigned long)(pc - base), (unsigned long)(lr - base));
    }
#else
    (void)ctx;
#endif
    void* frames[48];
    int n = backtrace(frames, 48);
    line(f, "stack (exe offsets; others are system libraries):\n");
    for (int i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)frames[i];
        if (a >= base && a < base + 0x10000000) {
            line(f, "  #%02d exe+0x%lx\n", i, (unsigned long)(a - base));
        } else {
            line(f, "  #%02d %p\n", i, frames[i]);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    // Let the system handle it (and write its own crash report).
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void crash_install(void) {
    static char altstack[64 * 1024];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}
#endif
