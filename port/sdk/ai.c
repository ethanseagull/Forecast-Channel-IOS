// AI, the Wii's audio interface, on SDL3 audio.
//
// On the Wii the AI plays the DSP's 32 kHz output and interrupts every 3 ms
// (96 samples), when it needs the next buffer; AX does all of its per-frame
// work in that interrupt (the AI DMA callback). Here a frame thread does the
// same: once per 96-sample frame it calls the DMA callback with "interrupts
// disabled" (OSDisableInterrupts, the port's global lock), then queues the
// buffer last given to AIInitDMA on an SDL audio stream (32 kHz stereo s16;
// SDL converts to the device's rate).
//
// Pacing: frames are produced on a 3 ms clock, so nw4r's sound thread gets
// its message and runs between frames as on the Wii, instead of in bursts the
// size of the device's buffer. The stream keeps the device's buffer plus about
// two frames queued; the clock is nudged (a frame skipped or added) when the
// queue drifts out of that range, which absorbs the drift between the system
// clock and the device's. Without a device (none, or FORECAST_MUTE=1) the
// clock still runs, since the channel's sound logic is driven by it.
//
// Buffers handed to AIInitDMA are native-endian s16, interleaved L,R.
#include <SDL3/SDL.h>
#include <string.h>

#include <revolution/AI.h>
#include <revolution/OS.h>

#include <port_audio.h>
#include <port_log.h>
#include <port_mem.h>

#define FRAME_SAMPLES 96
#define FRAME_BYTES (FRAME_SAMPLES * 2 * (int)sizeof(s16))
#define FRAME_NS 3000000ull // 96 samples at 32 kHz

static BOOL sInit;
static AIDMACallback sCallback;
static const void* sDmaBuf;
static u32 sDmaLen;
static u32 sDspRate = AI_DSP_32KHZ;

static int sOffline;
static BOOL sStarted;
static SDL_Thread* sThread;
static SDL_AtomicInt sQuit;
static SDL_AudioStream* sStream;
static BOOL sAudioSubsystem;
static u64 sFrames;
// Frames kept queued in the stream: prefill / low / high marks.
static int sTarget = 6, sLow = 1, sHigh = 8;
// Statistics (frame thread): queue range after the prefill, corrections.
static int sQueuedMin = 1 << 30, sQueuedMax;
static u32 sRefills, sSkips;

// ------------------------------------------------------------------ SDK API

void AIInit(void* stack) {
    (void)stack;
    sInit = TRUE;
}

BOOL AICheckInit(void) { return sInit; }

AIDMACallback AIRegisterDMACallback(AIDMACallback callback) {
    BOOL enabled = OSDisableInterrupts();
    AIDMACallback old = sCallback;
    sCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

void AIInitDMA(void* buffer, u32 length) {
    BOOL enabled = OSDisableInterrupts();
    sDmaBuf = buffer;
    sDmaLen = length;
    OSRestoreInterrupts(enabled);
}

u32 AIGetDMABytesLeft(void) { return 0; }
u32 AIGetDMAStartAddr(void) { return sDmaBuf != NULL ? port_gx_addr(sDmaBuf) : 0; }
u32 AIGetDMALength(void) { return sDmaLen; }

// The output is always 32 kHz (AX's rate); 48 kHz only mattered for the
// GameCube's disc streaming.
void AISetDSPSampleRate(u32 rate) { sDspRate = rate; }
u32 AIGetDSPSampleRate(void) { return sDspRate; }

void __AIDHandler(s16 intr, OSContext* ctx) {
    (void)intr;
    (void)ctx;
}

// ------------------------------------------------------------------ frames

// One "AI interrupt": the DMA callback, then the frame's samples.
static void tick(s16* out) {
    BOOL enabled = OSDisableInterrupts();
    if (sCallback != NULL) {
        sCallback();
    }
    int n = sDmaBuf != NULL ? (int)(sDmaLen < (u32)FRAME_BYTES ? sDmaLen : (u32)FRAME_BYTES) : 0;
    if (out != NULL) {
        if (n > 0) {
            memcpy(out, sDmaBuf, (size_t)n);
        }
        memset((u8*)out + n, 0, (size_t)(FRAME_BYTES - n));
        // Debugging: FORECAST_AUDIO_RAW=path appends every frame (s16
        // stereo, 32 kHz, native endian) to a raw PCM file.
        static int sCaptureInit;
        static SDL_IOStream* sCapture;
        if (!sCaptureInit) {
            sCaptureInit = 1;
            const char* path = SDL_getenv("FORECAST_AUDIO_RAW");
            if (path != NULL && path[0] != '\0') {
                sCapture = SDL_IOFromFile(path, "wb");
                port_log("AI: capturing output to %s%s\n", path, sCapture != NULL ? "" : " (failed)");
            }
        }
        if (sCapture != NULL) {
            SDL_WriteIO(sCapture, out, FRAME_BYTES);
            SDL_FlushIO(sCapture);
        }
    }
    sFrames++;
    OSRestoreInterrupts(enabled);
}

static int frame_thread(void* arg) {
    (void)arg;
    s16 buf[FRAME_SAMPLES * 2];
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
    // nw4r::snd's frame callback runs here: keep any operator new it does off
    // the channel's (single-threaded) MEM heaps.
    port_mark_host_thread();

    if (sStream != NULL) {
        for (int i = 0; i < sTarget; i++) {
            tick(buf);
            SDL_PutAudioStreamData(sStream, buf, FRAME_BYTES);
        }
    }

    Uint64 next = SDL_GetTicksNS();
    while (!SDL_GetAtomicInt(&sQuit)) {
        Uint64 now = SDL_GetTicksNS();
        if (now < next) {
            SDL_DelayNS(next - now);
            continue;
        }
        if (now > next + 100 * 1000000ull) {
            next = now; // stalled (debugger, suspended app): do not race to catch up
        }
        int produce = 1;
        if (sStream != NULL) {
            int queued = SDL_GetAudioStreamQueued(sStream) / FRAME_BYTES;
            sQueuedMin = queued < sQueuedMin ? queued : sQueuedMin;
            sQueuedMax = queued > sQueuedMax ? queued : sQueuedMax;
            if (queued > sHigh) {
                produce = 0; // the device runs slower than our clock
                sSkips++;
            } else if (queued < sLow) {
                produce = sTarget - queued; // ran dry or faster device: refill
                sRefills++;
            }
        }
        for (int i = 0; i < produce; i++) {
            tick(buf);
            if (sStream != NULL) {
                SDL_PutAudioStreamData(sStream, buf, FRAME_BYTES);
            }
        }
        next += FRAME_NS;
    }
    return 0;
}

static void open_device(void) {
    const char* mute = SDL_getenv("FORECAST_MUTE");
    if (mute != NULL && mute[0] != '\0' && mute[0] != '0') {
        port_log("AI: muted (FORECAST_MUTE); frames still run\n");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        port_log("AI: no audio (%s); frames still run\n", SDL_GetError());
        return;
    }
    sAudioSubsystem = TRUE;

    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32000};
    sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (sStream == NULL) {
        port_log("AI: cannot open the audio device (%s); frames still run\n", SDL_GetError());
        return;
    }

    // Queue the device's buffer (in AX frames, rounded up) plus two frames.
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(sStream);
    SDL_AudioSpec devSpec;
    int devFrames = 0;
    int chunk = 4;
    if (SDL_GetAudioDeviceFormat(dev, &devSpec, &devFrames) && devFrames > 0 && devSpec.freq > 0) {
        chunk = (int)(((s64)devFrames * 32000 / devSpec.freq + FRAME_SAMPLES - 1) / FRAME_SAMPLES);
    }
    sTarget = chunk + 2;
    sLow = 1;
    sHigh = sTarget + 2;

    SDL_ResumeAudioStreamDevice(sStream);
    port_log("AI: %s, device %d Hz, %d-sample buffer; %d frames (%d ms) queued\n",
             SDL_GetAudioDeviceName(dev) ? SDL_GetAudioDeviceName(dev) : "?", devSpec.freq, devFrames, sTarget,
             sTarget * 3);
}

void AIStartDMA(void) {
    if (sStarted) {
        return;
    }
    sStarted = TRUE;
    if (sOffline) {
        return;
    }
    open_device();
    SDL_SetAtomicInt(&sQuit, 0);
    sThread = SDL_CreateThread(frame_thread, "AI", NULL);
    if (sThread == NULL) {
        port_log("AI: cannot create the frame thread (%s)\n", SDL_GetError());
    }
}

// ------------------------------------------------------------------ port API

void port_audio_set_offline(int offline) { sOffline = offline; }

void port_audio_render(int16_t* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        tick(out != NULL ? out + (size_t)i * FRAME_SAMPLES * 2 : NULL);
    }
}

uint64_t port_audio_frame_count(void) {
    BOOL enabled = OSDisableInterrupts();
    u64 n = sFrames;
    OSRestoreInterrupts(enabled);
    return n;
}

void port_audio_stats(int* queuedMin, int* queuedMax, uint32_t* refills, uint32_t* skips) {
    *queuedMin = sQueuedMin;
    *queuedMax = sQueuedMax;
    *refills = sRefills;
    *skips = sSkips;
}

void port_audio_shutdown(void) {
    if (sThread != NULL) {
        SDL_SetAtomicInt(&sQuit, 1);
        SDL_WaitThread(sThread, NULL);
        sThread = NULL;
    }
    if (sStream != NULL) {
        SDL_DestroyAudioStream(sStream);
        sStream = NULL;
    }
    if (sAudioSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sAudioSubsystem = FALSE;
    }
    sStarted = FALSE;
}
