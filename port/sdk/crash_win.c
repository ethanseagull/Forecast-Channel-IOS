// Windows crash reports: on an unhandled exception, logs the code, the faulting
// address and a stack walk as offsets into the executable, ready for
//   llvm-symbolizer --obj=forecast.exe 0x<offset> ...
// Installed by a constructor that runs before the decomp's static
// initialisers (which may crash before main).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void report(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    OutputDebugStringA(buf);
    FILE* f = fopen("crash.txt", "a");
    if (f != NULL) {
        fputs(buf, f);
        fclose(f);
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    // Only real faults (thread naming, debug output etc. are informational).
    if (er->ExceptionCode < 0xC0000000u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD64 exeBase = (DWORD64)GetModuleHandleW(NULL);
    CONTEXT ctx = *ep->ContextRecord;

    report("\n*** crash: exception 0x%08lX at %p", er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        report(" (%s %p)", er->ExceptionInformation[0] == 1 ? "write to" : "read from",
               (void*)er->ExceptionInformation[1]);
    }
    report("\nstack (exe offsets; symbolize with llvm-symbolizer --obj=forecast.exe):\n");
    for (int i = 0; i < 40 && ctx.Rip != 0; i++) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, NULL);
        if (imageBase == exeBase) {
            report("  #%02d 0x%llx\n", i, (unsigned long long)(ctx.Rip - exeBase));
        } else {
            char mod[MAX_PATH] = "?";
            if (imageBase != 0) {
                GetModuleFileNameA((HMODULE)imageBase, mod, sizeof(mod));
            }
            const char* name = strrchr(mod, '\\');
            report("  #%02d %s+0x%llx\n", i, name ? name + 1 : mod, (unsigned long long)(ctx.Rip - imageBase));
        }
        if (fn == NULL) {
            // Leaf function: the return address is on top of the stack.
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, NULL);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- integer division by zero -------------------------------------------------
// PowerPC's divw/divwu (the Wii) and arm64's sdiv/udiv (iOS) do not trap on a
// zero divisor; x86's div/idiv do, and the channel relies on not trapping in a
// few places. Emulate arm64: quotient 0, remainder = dividend, then skip the
// instruction. Each site is logged once.

// Length of a div/idiv (F6 /6,/7 or F7 /6,/7, any prefixes / REX) at p, or 0.
static int div_insn_length(const unsigned char* p) {
    const unsigned char* q = p;
    int addr32 = 0;
    for (;;) {
        unsigned char b = *q;
        if (b == 0x66 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65) {
            q++;
        } else if (b == 0x67) {
            addr32 = 1;
            q++;
        } else {
            break;
        }
    }
    if ((*q & 0xF0) == 0x40) { // REX
        q++;
    }
    if (*q != 0xF6 && *q != 0xF7) {
        return 0;
    }
    q++;
    unsigned char modrm = *q++;
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 6 && reg != 7) {
        return 0;
    }
    (void)addr32;
    if (mod != 3) {
        if (rm == 4) {
            unsigned char sib = *q++;
            if (mod == 0 && (sib & 7) == 5) {
                q += 4;
            }
        } else if (mod == 0 && rm == 5) {
            q += 4; // RIP-relative
        }
        q += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    }
    return (int)(q - p);
}

static LONG WINAPI divzero_handler(EXCEPTION_POINTERS* ep) {
    static void* sSeen[64];
    static int sSeenCount;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = ep->ContextRecord;
    int len = div_insn_length((const unsigned char*)c->Rip);
    if (len == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void* site = (void*)c->Rip;
    int seen = 0;
    for (int i = 0; i < sSeenCount; i++) {
        seen |= sSeen[i] == site;
    }
    if (!seen && sSeenCount < 64) {
        sSeen[sSeenCount++] = site;
        report("divide by zero at exe+0x%llx: arm64 result used (quotient 0)\n",
               (unsigned long long)(c->Rip - (DWORD64)GetModuleHandleW(NULL)));
    }
    c->Rdx = c->Rax; // remainder = dividend (low part)
    c->Rax = 0;      // quotient = 0
    c->Rip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void __cdecl crash_install(void) {
    AddVectoredExceptionHandler(1, divzero_handler);
    SetUnhandledExceptionFilter(crash_filter);
}

// .CRT$XCB runs before every C++ static initialiser (.CRT$XCU).
#pragma section(".CRT$XCB", read)
__declspec(allocate(".CRT$XCB")) void(__cdecl* port_crash_install_entry)(void) = crash_install;
#endif
