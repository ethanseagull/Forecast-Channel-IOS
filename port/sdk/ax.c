// AX, the Wii's DSP sound mixer, on the host.
//
// Two halves, as on the Wii:
//  - The CPU library: voice allocation with priorities and the drop callback,
//    AXSetVoice* writing the voice's parameter block (vpb->pb) and sync flags,
//    the per-frame sync of those blocks into the "DSP's" copies, the DSP cycle
//    budget, depop, aux and user callbacks. This follows the SDK itself
//    (forecast-decomp/src/revolution/AX).
//  - The DSP's work on its copies (sDspPB): ADPCM / PCM16 / PCM8 decoding with
//    loops and end detection, sample-rate conversion, volume envelope, low-pass
//    and biquad filters, mixing into the main and aux buses, aux returns,
//    surround fold-down and master volume. This follows Dolphin's AX HLE
//    (DSPHLE/UCodes/AXVoice.h, AXWii.cpp) and its accelerator (DSPAccelerator.cpp).
//
// Timing: ai.c calls the AI DMA callback once per 96-sample frame with the
// port's "interrupts disabled" lock held, like the Wii's AI interrupt. AXInit
// registers ax_frame as that callback: sync, drop callbacks, mix (aux
// callbacks run here), then the AXRegisterCallback callback, then the frame is
// handed to the AI with AIInitDMA. Everything the game thread does through the
// AX API also holds that lock, so a frame always sees whole updates.
//
// Differences from the Wii (all deliberate):
//  - Volume ramps (AXPBVE delta, AXPBMIX deltas) last one frame: the deltas
//    are applied during the first frame the voice plays after they were synced
//    and then cleared, so the volume holds at the ramp's end. The real DSP keeps
//    ramping until the CPU writes new values; nw4r always sends one-frame ramps
//    and re-sends every frame, so the two agree whenever the sound thread keeps
//    up. When it falls a frame behind (host scheduling), the Wii behaviour would
//    overshoot or wrap the 16-bit volume (a loud click); holding does not.
//  - The 4-tap polyphase SRC uses coefficients generated here (Lanczos-windowed
//    sinc, cutoffs 8/12/16 kHz) instead of the DSP ROM's tables.
//  - Aux sends are processed in the frame they were mixed (AX returns them two
//    frames later through its ring of three buffers).
//  - No compressor (AX's limiter that acts only when the output clips): the
//    output is clamped instead, as in Dolphin.
//  - DPL2 and "surround" output are folded down to stereo; ITD (initial time
//    delay) and the Wii Remote speaker mix are not produced.
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <revolution/AI.h>
#include <revolution/AX.h>
#include <revolution/OS.h>

#include <port_mem.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NSAMP AX_SAMPLES_PER_FRAME // 96
#define STACK_MAX (AX_PRIORITY_MAX + 1)

// ------------------------------------------------------------------ state

static BOOL sInit;

static AXVPB sVPB[AX_VOICE_MAX];   // the game's voices
static AXPB sDspPB[AX_VOICE_MAX];  // the DSP's copies
static u8 sItd[AX_VOICE_MAX][0x40]; // ITD buffers (unused, but vpb->itdBuffer points somewhere)

static AXVPB* sStackHead[STACK_MAX];
static AXVPB* sStackTail[STACK_MAX];
static AXVPB* sCallbackStack;

static u32 sMaxDspCycles;
static u32 sRecDspCycles;
static s32 sNumVoices;

static AXOutCallback sUserCallback;
static AXAuxCallback sAuxCallback[3];
static void* sAuxContext[3];

static u32 sMode = AX_OUTPUT_STEREO;
static u16 sMasterVolume = AX_MAX_VOLUME;
static u16 sAuxReturnVolume[3] = {AX_MAX_VOLUME, AX_MAX_VOLUME, AX_MAX_VOLUME};
// Volumes used by the previous frame (the DSP ramps to the new ones).
static u16 sLastMasterVolume = AX_MAX_VOLUME;
static u16 sLastAuxReturnVolume[3] = {AX_MAX_VOLUME, AX_MAX_VOLUME, AX_MAX_VOLUME};

// Buses: main L R S, aux A L R S, aux B L R S, aux C L R S.
enum { BUS_L, BUS_R, BUS_S, BUS_AL, BUS_AR, BUS_AS, BUS_BL, BUS_BR, BUS_BS, BUS_CL, BUS_CR, BUS_CS, BUS_COUNT };
static s32 sBus[BUS_COUNT][NSAMP];

// Depop ("studio"): the last sample of voices stopped while playing, faded
// out over the next frames instead of cut (AXSPB.c).
static s32 sDepopAll[BUS_COUNT];

// The frame handed to the AI: native s16, L,R interleaved.
static s16 sOut[NSAMP * 2];

// SRC: 3 filters (8, 12, 16 kHz) x 128 phases x 4 taps, Q15.
static s32 sSrcCoef[3][128][4];

// ------------------------------------------------------------------ helpers

static inline s16 clamp16(s64 v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

static inline u32 hilo(u16 hi, u16 lo) { return ((u32)hi << 16) | lo; }

static inline u8 gx_read8(u32 addr) {
    const u8* p = (const u8*)port_gx_ptr(addr);
    return p != NULL ? *p : 0;
}

// ------------------------------------------------------------------ voice stacks (AXAlloc.c)

static void push_free_stack(AXVPB* vpb) {
    vpb->next = sStackHead[AX_PRIORITY_FREE];
    sStackHead[AX_PRIORITY_FREE] = vpb;
    vpb->priority = AX_PRIORITY_FREE;
}

static AXVPB* pop_free_stack(void) {
    AXVPB* head = sStackHead[AX_PRIORITY_FREE];
    if (head != NULL) {
        sStackHead[AX_PRIORITY_FREE] = (AXVPB*)head->next;
    }
    return head;
}

static void push_callback_stack(AXVPB* vpb) {
    vpb->next1 = sCallbackStack;
    sCallbackStack = vpb;
}

static AXVPB* pop_callback_stack(void) {
    AXVPB* head = sCallbackStack;
    if (head != NULL) {
        sCallbackStack = (AXVPB*)head->next1;
    }
    return head;
}

static void remove_from_stack(AXVPB* vpb) {
    u32 prio = vpb->priority;
    AXVPB* head = sStackHead[prio];
    AXVPB* tail = sStackTail[prio];

    if (head == tail) {
        sStackHead[prio] = sStackTail[prio] = NULL;
    } else if (vpb == head) {
        sStackHead[prio] = (AXVPB*)vpb->next;
        sStackHead[prio]->prev = NULL;
    } else if (vpb == tail) {
        sStackTail[prio] = (AXVPB*)vpb->prev;
        sStackTail[prio]->next = NULL;
    } else {
        AXVPB* p = (AXVPB*)vpb->prev;
        AXVPB* n = (AXVPB*)vpb->next;
        p->next = n;
        n->prev = p;
    }
}

static void push_stack_head(AXVPB* vpb, u32 prio) {
    vpb->next = sStackHead[prio];
    vpb->prev = NULL;
    if (vpb->next != NULL) {
        sStackHead[prio]->prev = vpb;
    } else {
        sStackTail[prio] = vpb;
    }
    sStackHead[prio] = vpb;
    vpb->priority = prio;
}

static AXVPB* pop_stack_from_bottom(u32 prio) {
    AXVPB* vpb = NULL;
    if (sStackHead[prio] != NULL) {
        if (sStackHead[prio] == sStackTail[prio]) {
            vpb = sStackHead[prio];
            sStackHead[prio] = sStackTail[prio] = NULL;
        } else if (sStackTail[prio] != NULL) {
            vpb = sStackTail[prio];
            sStackTail[prio] = (AXVPB*)vpb->prev;
            sStackTail[prio]->next = NULL;
        }
    }
    return vpb;
}

static void set_pb_default(AXVPB* vpb) {
    vpb->pb.state = AX_VOICE_STOP;
    vpb->pb.itd.flag = 0;
    vpb->sync = AX_PBSYNC_STATE | AX_PBSYNC_ITD | AX_PBSYNC_LPF | AX_PBSYNC_BIQUAD | AX_PBSYNC_REMOTE |
                AX_PBSYNC_RMTSRC | AX_PBSYNC_RMTIIR;
    vpb->pb.lpf.on = 0;
    vpb->pb.biquad.on = 0;
    vpb->pb.remote = 0;
    vpb->pb.rmtIIR.lpf.on = 0;
    memset(&vpb->pb.rmtSrc, 0, sizeof(vpb->pb.rmtSrc));
}

static void service_callback_stack(void) {
    AXVPB* it;
    while ((it = pop_callback_stack()) != NULL) {
        if (it->priority > AX_PRIORITY_FREE) {
            if (it->callback != NULL) {
                it->callback(it);
            }
            remove_from_stack(it);
            push_free_stack(it);
        }
    }
}

void AXFreeVoice(AXVPB* vpb) {
    BOOL enabled = OSDisableInterrupts();
    remove_from_stack(vpb);
    if (vpb->pb.state == AX_VOICE_RUN) {
        vpb->depop = TRUE;
    }
    set_pb_default(vpb);
    push_free_stack(vpb);
    OSRestoreInterrupts(enabled);
}

AXVPB* AXAcquireVoice(u32 prio, AXVoiceCallback callback, u32 userContext) {
    BOOL enabled = OSDisableInterrupts();
    AXVPB* vpb = pop_free_stack();

    if (vpb == NULL) {
        // Steal the oldest voice of the lowest priority below `prio`.
        for (u32 i = AX_PRIORITY_MIN; i < prio; i++) {
            vpb = pop_stack_from_bottom(i);
            if (vpb != NULL) {
                if (vpb->pb.state == AX_VOICE_RUN) {
                    vpb->depop = TRUE;
                }
                if (vpb->callback != NULL) {
                    vpb->callback(vpb);
                }
                break;
            }
        }
    }

    if (vpb != NULL) {
        push_stack_head(vpb, prio);
        vpb->callback = callback;
        vpb->userContext = userContext;
        set_pb_default(vpb);
    }

    OSRestoreInterrupts(enabled);
    return vpb;
}

void AXSetVoicePriority(AXVPB* vpb, u32 prio) {
    BOOL enabled = OSDisableInterrupts();
    remove_from_stack(vpb);
    push_stack_head(vpb, prio);
    OSRestoreInterrupts(enabled);
}

// ------------------------------------------------------------------ AXSetVoice* (AXVPB.c)

void AXSetVoiceSrcType(AXVPB* vpb, u32 type) {
    BOOL enabled = OSDisableInterrupts();
    AXPB* pb = &vpb->pb;
    switch (type) {
    case AX_SRC_TYPE_NONE: pb->srcSelect = 2; break;
    case AX_SRC_TYPE_LINEAR: pb->srcSelect = 1; break;
    case AX_SRC_TYPE_4TAP_8K: pb->srcSelect = 0; pb->coefSelect = 0; break;
    case AX_SRC_TYPE_4TAP_12K: pb->srcSelect = 0; pb->coefSelect = 1; break;
    case AX_SRC_TYPE_4TAP_16K: pb->srcSelect = 0; pb->coefSelect = 2; break;
    default: break;
    }
    vpb->sync |= AX_PBSYNC_SELECT;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceState(AXVPB* vpb, u16 state) {
    BOOL enabled = OSDisableInterrupts();
    if (vpb->pb.state != state) {
        vpb->pb.state = state;
        vpb->sync |= AX_PBSYNC_STATE;
        if (state == AX_VOICE_STOP) {
            vpb->depop = TRUE;
        }
    }
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceType(AXVPB* vpb, u16 type) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.type = type;
    vpb->sync |= AX_PBSYNC_TYPE;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceMix(AXVPB* vpb, AXPBMIX* mix) {
    // A field that is nonzero enables its bus (and a delta its ramp); the bits
    // are those AXSetVoiceMix computes on the Wii.
    static const u32 kBits[24] = {
        0x1,      0x5,      0x2,      0x6,      0x10000,    0x50000,    0x20000,   0x60000,
        0x200000, 0xA00000, 0x400000, 0xC00000, 0x4000000,  0x14000000, 0x8000000, 0x18000000,
        0x8,      0x18,     0x80000,  0x180000, 0x1000000,  0x3000000,  0x20000000, 0x60000000,
    };
    BOOL enabled = OSDisableInterrupts();
    const u16* src = (const u16*)mix;
    u16* dst = (u16*)&vpb->pb.mix;
    u32 mixerCtrl = 0;
    for (int i = 0; i < 24; i++) {
        if ((dst[i] = src[i]) != 0) {
            mixerCtrl |= kBits[i];
        }
    }
    vpb->pb.mixerCtrl = mixerCtrl;
    vpb->sync |= AX_PBSYNC_MIXER_CTRL | AX_PBSYNC_MIX;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceVe(AXVPB* vpb, AXPBVE* ve) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.ve.currentVolume = ve->currentVolume;
    vpb->pb.ve.currentDelta = ve->currentDelta;
    vpb->sync |= AX_PBSYNC_VE;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceAddr(AXVPB* vpb, AXPBADDR* addr) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.addr = *addr;
    // PCM needs unity gain and no prediction (the SDK sets these here).
    if (addr->format == AX_SAMPLE_FORMAT_PCM_S16 || addr->format == AX_SAMPLE_FORMAT_PCM_S8) {
        memset(vpb->pb.adpcm.a, 0, sizeof(vpb->pb.adpcm.a));
        vpb->pb.adpcm.gain = addr->format == AX_SAMPLE_FORMAT_PCM_S16 ? 0x0800 : 0x0100;
        vpb->pb.adpcm.pred_scale = 0;
        vpb->pb.adpcm.yn1 = 0;
        vpb->pb.adpcm.yn2 = 0;
    }
    vpb->sync &= ~(AX_PBSYNC_LOOP_FLAG | AX_PBSYNC_LOOP_ADDR | AX_PBSYNC_END_ADDR | AX_PBSYNC_CURR_ADDR);
    vpb->sync |= AX_PBSYNC_ADDR | AX_PBSYNC_ADPCM;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceAdpcm(AXVPB* vpb, AXPBADPCM* adpcm) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.adpcm = *adpcm;
    vpb->sync |= AX_PBSYNC_ADPCM;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceSrc(AXVPB* vpb, AXPBSRC* src) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.src = *src;
    vpb->sync &= ~AX_PBSYNC_SRC_RATIO;
    vpb->sync |= AX_PBSYNC_SRC;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceSrcRatio(AXVPB* vpb, f32 ratio) {
    BOOL enabled = OSDisableInterrupts();
    u32 r = (u32)(65536.0f * ratio);
    vpb->pb.src.ratioHi = (u16)(r >> 16);
    vpb->pb.src.ratioLo = (u16)r;
    vpb->sync |= AX_PBSYNC_SRC_RATIO;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceAdpcmLoop(AXVPB* vpb, AXPBADPCMLOOP* loop) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.adpcmLoop = *loop;
    vpb->sync |= AX_PBSYNC_ADPCM_LOOP;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceLpf(AXVPB* vpb, AXPBLPF* lpf) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.lpf = *lpf;
    vpb->sync |= AX_PBSYNC_LPF;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceLpfCoefs(AXVPB* vpb, u16 a0, u16 b0) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.lpf.a0 = a0;
    vpb->pb.lpf.b0 = b0;
    vpb->sync |= AX_PBSYNC_LPF_COEFS;
    OSRestoreInterrupts(enabled);
}

void AXGetLpfCoefs(u16 freq, u16* a, u16* b) {
    f32 c = 2.0f - (f32)cos(2 * M_PI * freq / AX_SAMPLE_RATE);
    f32 r = (f32)sqrt(c * c - 1.0f) - c;
    *b = (u16)(32768 * -r);
    *a = (u16)(32767 - *b);
}

// The Wii Remote speaker mix is kept in the parameter block but not produced.
void AXSetVoiceRmtOn(AXVPB* vpb, u16 on) {
    BOOL enabled = OSDisableInterrupts();
    vpb->pb.remote = on;
    vpb->sync |= AX_PBSYNC_REMOTE;
    OSRestoreInterrupts(enabled);
}

void AXSetVoiceRmtMix(AXVPB* vpb, AXPBRMTMIX* mix) {
    BOOL enabled = OSDisableInterrupts();
    const u16* src = (const u16*)mix;
    u16* dst = (u16*)&vpb->pb.rmtMix;
    u16 ctrl = 0;
    for (int i = 0; i < 16; i++) {
        if ((dst[i] = src[i]) != 0) {
            ctrl |= (u16)(1u << i);
        }
    }
    vpb->pb.rmtMixerCtrl = ctrl;
    vpb->sync |= AX_PBSYNC_RMT_MIXER_CTRL | AX_PBSYNC_RMTMIX;
    OSRestoreInterrupts(enabled);
}

void AXSetMaxDspCycles(u32 num) { sMaxDspCycles = num; }

s32 AXGetMaxVoices(void) { return AX_VOICE_MAX; }

// Remote speaker output: never any samples, so nw4r's speaker alarm idles.
s32 AXRmtGetSamplesLeft(void) { return 0; }

s32 AXRmtGetSamples(s32 chan, s16* out, s32 num) {
    (void)chan;
    (void)out;
    (void)num;
    return 0;
}

s32 AXRmtAdvancePtr(s32 num) {
    (void)num;
    return 0;
}

// ------------------------------------------------------------------ output settings (AXCL.c, AXAux.c, AXOut.c)

void AXSetMode(u32 mode) { sMode = mode; }
u32 AXGetMode(void) { return sMode; }

void AXSetMasterVolume(u16 volume) {
    sMasterVolume = volume > AX_MAX_VOLUME ? AX_MAX_VOLUME : volume;
}

void AXSetAuxAReturnVolume(u16 volume) { sAuxReturnVolume[0] = volume; }
void AXSetAuxBReturnVolume(u16 volume) { sAuxReturnVolume[1] = volume; }
void AXSetAuxCReturnVolume(u16 volume) { sAuxReturnVolume[2] = volume; }
u16 AXGetAuxAReturnVolume(void) { return sAuxReturnVolume[0]; }
u16 AXGetAuxBReturnVolume(void) { return sAuxReturnVolume[1]; }
u16 AXGetAuxCReturnVolume(void) { return sAuxReturnVolume[2]; }

static void register_aux(int bus, AXAuxCallback callback, void* context) {
    BOOL enabled = OSDisableInterrupts();
    sAuxCallback[bus] = callback;
    sAuxContext[bus] = context;
    OSRestoreInterrupts(enabled);
}

void AXRegisterAuxACallback(AXAuxCallback callback, void* context) { register_aux(0, callback, context); }
void AXRegisterAuxBCallback(AXAuxCallback callback, void* context) { register_aux(1, callback, context); }
void AXRegisterAuxCCallback(AXAuxCallback callback, void* context) { register_aux(2, callback, context); }

void AXGetAuxACallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[0];
    *context = sAuxContext[0];
}
void AXGetAuxBCallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[1];
    *context = sAuxContext[1];
}
void AXGetAuxCCallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[2];
    *context = sAuxContext[2];
}

AXOutCallback AXRegisterCallback(AXOutCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    AXOutCallback old = sUserCallback;
    sUserCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

// ------------------------------------------------------------------ per-frame sync (AXVPB.c __AXSyncPBs)

static const u32 kMixCycles[32] = {2,   408,  408,  810,  1404, 1404, 1404, 1404, 408, 816,  816,
                                   1218, 1812, 1812, 1812, 1812, 707, 1115, 1115, 1517, 2111, 2111,
                                   2111, 2111, 707, 1115, 1115, 1517, 2111, 2111, 2111, 2111};
static const u32 kRmtMixCycles[4] = {4, 86, 151, 151};

static u32 src_cycles(u16 select, const AXPBSRC* src) {
    u32 ratio = hilo(src->ratioHi, src->ratioLo);
    if (select == 0) {
        return (ratio * 512 + 32768) / 65536 + 1561;
    }
    if (select == 1) {
        return (ratio * 512 + 32768) / 65536 + 1466;
    }
    return 605;
}

// The command list's cost, as __AXNextFrame counts it.
static u32 command_list_cycles(void) {
    u32 c = 7811 + 4126;
    c += sMode == AX_OUTPUT_STEREO ? 733 : sMode == AX_OUTPUT_SURROUND ? 829 : 925;
    if (sMode == AX_OUTPUT_DPL2) {
        c += (sAuxCallback[0] ? 3036 : 0) + (sAuxCallback[1] ? 3036 : 0);
    } else {
        c += (sAuxCallback[0] ? 2235 : 0) + (sAuxCallback[1] ? 2235 : 0) + (sAuxCallback[2] ? 2235 : 0);
    }
    c += 1850 + 409; // compressor, remote output
    c += sMode == AX_OUTPUT_DPL2 ? 1195 : 1172;
    return c + 30;
}

static void depop_voice(AXPB* pb) {
    sDepopAll[BUS_L] += pb->dpop.aL;
    sDepopAll[BUS_R] += pb->dpop.aR;
    sDepopAll[BUS_S] += pb->dpop.aS;
    sDepopAll[BUS_AL] += pb->dpop.aAuxAL;
    sDepopAll[BUS_AR] += pb->dpop.aAuxAR;
    sDepopAll[BUS_AS] += pb->dpop.aAuxAS;
    sDepopAll[BUS_BL] += pb->dpop.aAuxBL;
    sDepopAll[BUS_BR] += pb->dpop.aAuxBR;
    sDepopAll[BUS_BS] += pb->dpop.aAuxBS;
    sDepopAll[BUS_CL] += pb->dpop.aAuxCL;
    sDepopAll[BUS_CR] += pb->dpop.aAuxCR;
    sDepopAll[BUS_CS] += pb->dpop.aAuxCS;
    // Counted once (the SDK would add the same values again on a second stop).
    memset(&pb->dpop, 0, sizeof(pb->dpop));
}

// Copies what the game changed (vpb->sync) to the DSP's block, and the DSP's
// progress (state, volume, current address) back to the game's.
static void service_vpb(AXVPB* vpb) {
    AXPB* dst = &sDspPB[vpb->index];
    AXPB* src = &vpb->pb;
    u32 sync = vpb->sync;

    sNumVoices++;

    if (sync == 0) {
        src->state = dst->state;
        src->ve.currentVolume = dst->ve.currentVolume;
        src->addr.currentAddressHi = dst->addr.currentAddressHi;
        src->addr.currentAddressLo = dst->addr.currentAddressLo;
        return;
    }

    if (sync & AX_PBSYNC_ALL) {
        *dst = *src;
        return;
    }

    if (sync & AX_PBSYNC_SELECT) {
        dst->srcSelect = src->srcSelect;
        dst->coefSelect = src->coefSelect;
    }
    if (sync & AX_PBSYNC_MIXER_CTRL) {
        dst->mixerCtrl = src->mixerCtrl;
    }
    if (sync & AX_PBSYNC_STATE) {
        dst->state = src->state;
    } else {
        src->state = dst->state;
    }
    if (sync & AX_PBSYNC_TYPE) {
        dst->type = src->type;
    }
    if (sync & AX_PBSYNC_MIX) {
        dst->mix = src->mix;
    }
    if (sync & AX_PBSYNC_ITD_SHIFT) {
        dst->itd.targetShiftL = src->itd.targetShiftL;
        dst->itd.targetShiftR = src->itd.targetShiftR;
    } else if (sync & AX_PBSYNC_ITD) {
        dst->itd = src->itd;
        if (vpb->itdBuffer != NULL) {
            memset(vpb->itdBuffer, 0, sizeof(sItd[0]));
        }
    }
    if (sync & AX_PBSYNC_DPOP) {
        dst->dpop = src->dpop;
    }
    if (sync & AX_PBSYNC_VE_DELTA) {
        src->ve.currentVolume = dst->ve.currentVolume;
        dst->ve.currentDelta = src->ve.currentDelta;
    } else if (sync & AX_PBSYNC_VE) {
        dst->ve = src->ve;
    }

    if (sync & (AX_PBSYNC_LOOP_FLAG | AX_PBSYNC_LOOP_ADDR | AX_PBSYNC_END_ADDR | AX_PBSYNC_CURR_ADDR)) {
        if (sync & AX_PBSYNC_LOOP_FLAG) {
            dst->addr.loopFlag = src->addr.loopFlag;
        }
        if (sync & AX_PBSYNC_LOOP_ADDR) {
            dst->addr.loopAddressHi = src->addr.loopAddressHi;
            dst->addr.loopAddressLo = src->addr.loopAddressLo;
        }
        if (sync & AX_PBSYNC_END_ADDR) {
            dst->addr.endAddressHi = src->addr.endAddressHi;
            dst->addr.endAddressLo = src->addr.endAddressLo;
        }
        if (sync & AX_PBSYNC_CURR_ADDR) {
            dst->addr.currentAddressHi = src->addr.currentAddressHi;
            dst->addr.currentAddressLo = src->addr.currentAddressLo;
        } else {
            src->addr.currentAddressHi = dst->addr.currentAddressHi;
            src->addr.currentAddressLo = dst->addr.currentAddressLo;
        }
    } else if (sync & AX_PBSYNC_ADDR) {
        dst->addr = src->addr;
    } else {
        src->addr.currentAddressHi = dst->addr.currentAddressHi;
        src->addr.currentAddressLo = dst->addr.currentAddressLo;
    }

    if (sync & AX_PBSYNC_ADPCM) {
        dst->adpcm = src->adpcm;
    }
    if (sync & AX_PBSYNC_SRC_RATIO) {
        dst->src.ratioHi = src->src.ratioHi;
        dst->src.ratioLo = src->src.ratioLo;
    } else if (sync & AX_PBSYNC_SRC) {
        dst->src = src->src;
    }
    if (sync & AX_PBSYNC_ADPCM_LOOP) {
        dst->adpcmLoop = src->adpcmLoop;
    }
    if (sync & AX_PBSYNC_LPF_COEFS) {
        dst->lpf.a0 = src->lpf.a0;
        dst->lpf.b0 = src->lpf.b0;
    } else if (sync & AX_PBSYNC_LPF) {
        dst->lpf = src->lpf;
    }
    if (sync & AX_PBSYNC_BIQUAD_COEFS) {
        dst->biquad.b0 = src->biquad.b0;
        dst->biquad.b1 = src->biquad.b1;
        dst->biquad.b2 = src->biquad.b2;
        dst->biquad.a1 = src->biquad.a1;
        dst->biquad.a2 = src->biquad.a2;
    } else if (sync & AX_PBSYNC_BIQUAD) {
        dst->biquad = src->biquad;
    }
    if (sync & AX_PBSYNC_REMOTE) {
        dst->remote = src->remote;
    }
    if (sync & AX_PBSYNC_RMT_MIXER_CTRL) {
        dst->rmtMixerCtrl = src->rmtMixerCtrl;
    }
    if (sync & AX_PBSYNC_RMTMIX) {
        dst->rmtMix = src->rmtMix;
    }
    if (sync & AX_PBSYNC_RMTDPOP) {
        dst->rmtDpop = src->rmtDpop;
    }
    if (sync & AX_PBSYNC_RMTSRC) {
        dst->rmtSrc = src->rmtSrc;
    }
    if (sync & AX_PBSYNC_RMTIIR_LPF_COEFS) {
        dst->rmtIIR.lpf.a0 = src->rmtIIR.lpf.a0;
        dst->rmtIIR.lpf.b0 = src->rmtIIR.lpf.b0;
    } else if (sync & AX_PBSYNC_RMTIIR_BIQUAD_COEFS) {
        dst->rmtIIR.biquad.b0 = src->rmtIIR.biquad.b0;
        dst->rmtIIR.biquad.b1 = src->rmtIIR.biquad.b1;
        dst->rmtIIR.biquad.b2 = src->rmtIIR.biquad.b2;
        dst->rmtIIR.biquad.a1 = src->rmtIIR.biquad.a1;
        dst->rmtIIR.biquad.a2 = src->rmtIIR.biquad.a2;
    } else if (sync & AX_PBSYNC_RMTIIR) {
        dst->rmtIIR = src->rmtIIR;
    }
}

// Over the DSP budget: the voice is stopped and its callback runs this frame.
static void dump_vpb(AXVPB* vpb) {
    AXPB* pb = &sDspPB[vpb->index];
    if (pb->state == AX_VOICE_RUN) {
        depop_voice(pb);
    }
    vpb->pb.state = AX_VOICE_STOP;
    pb->state = AX_VOICE_STOP;
    push_callback_stack(vpb);
}

static void sync_pbs(void) {
    u32 cycles = 32 + command_list_cycles() + AX_VOICE_MAX * 600 + 24400;
    sNumVoices = 0;

    for (u32 prio = AX_PRIORITY_MAX; prio > AX_PRIORITY_FREE; prio--) {
        for (AXVPB* v = sStackHead[prio]; v != NULL; v = (AXVPB*)v->next) {
            if (v->pb.itd.flag == 1) {
                cycles += 129;
            }
            if (v->depop) {
                depop_voice(&sDspPB[v->index]);
            }
            if (v->pb.state == AX_VOICE_RUN) {
                cycles += 387;
                if (v->pb.lpf.on) {
                    cycles += 309;
                }
                if (v->pb.biquad.on) {
                    cycles += 1024;
                }
                if (v->pb.itd.flag == 1) {
                    cycles += 27;
                }
                cycles += src_cycles(v->pb.srcSelect, &v->pb.src);
                cycles += kMixCycles[v->pb.mixerCtrl >> 0 & 31] + kMixCycles[v->pb.mixerCtrl >> 16 & 31] +
                          kMixCycles[v->pb.mixerCtrl >> 21 & 31] + kMixCycles[v->pb.mixerCtrl >> 26 & 31];
                if (v->pb.remote == TRUE) {
                    cycles += 613;
                    if (v->pb.rmtIIR.lpf.on == AX_PB_LPF_ON) {
                        cycles += 118;
                    } else if (v->pb.rmtIIR.biquad.on == AX_PB_BIQUAD_ON) {
                        cycles += 834;
                    }
                    for (int s = 0; s < 16; s += 2) {
                        cycles += kRmtMixCycles[v->pb.rmtMixerCtrl >> s & 3];
                    }
                }
                if (sMaxDspCycles > cycles) {
                    service_vpb(v);
                } else {
                    dump_vpb(v);
                }
            } else {
                service_vpb(v);
            }
            v->sync = 0;
            v->depop = FALSE;
        }
    }
    sRecDspCycles = cycles;

    for (AXVPB* v = sStackHead[AX_PRIORITY_FREE]; v != NULL; v = (AXVPB*)v->next) {
        if (v->depop) {
            depop_voice(&sDspPB[v->index]);
        }
        v->depop = FALSE;
        sDspPB[v->index].state = AX_VOICE_STOP;
    }
}

// ------------------------------------------------------------------ DSP: sample reading (DSPAccelerator.cpp)

typedef struct Accel {
    AXPB* pb;
    u32 cur, start, end;
    s32 yn1, yn2;
    u16 ps;
    BOOL stopped;
} Accel;

static s16 accel_read(Accel* a) {
    AXPB* pb = a->pb;
    s32 val = 0;
    u32 step = 2;

    if (a->stopped) {
        return 0;
    }

    switch (pb->addr.format) {
    case AX_SAMPLE_FORMAT_DSP_ADPCM: {
        // Nibble addresses; each 8-byte frame is a header byte (predictor and
        // scale) and 14 samples, so nibbles 0 and 1 of a frame are skipped.
        u8 b = gx_read8(a->cur >> 1);
        s32 nib = (a->cur & 1) ? (b & 0xF) : (b >> 4);
        if (nib >= 8) {
            nib -= 16;
        }
        u32 idx = (a->ps >> 4) & 7;
        s32 c1 = (s16)pb->adpcm.a[idx][0];
        s32 c2 = (s16)pb->adpcm.a[idx][1];
        val = clamp16((s64)nib * (1 << (a->ps & 0xF)) + ((0x400 + c1 * a->yn1 + c2 * a->yn2) >> 11));
        a->yn2 = a->yn1;
        a->yn1 = val;
        a->cur++;
        // The DSP's special cases for end addresses on a frame header.
        if ((a->end & 0xF) == 0 && a->cur == a->end) {
            a->cur = a->start + 1;
        } else if ((a->end & 0xF) == 1 && a->cur == a->end - 1) {
            a->cur = a->start;
        } else if ((a->cur & 15) == 0) {
            a->ps = gx_read8((a->cur & ~15u) >> 1);
            a->cur += 2;
            step += 2;
        }
        break;
    }
    case AX_SAMPLE_FORMAT_PCM_S16: {
        // 16-bit word addresses; big-endian samples.
        const u8* p = (const u8*)port_gx_ptr(a->cur * 2);
        s32 raw = p != NULL ? (s16)((p[0] << 8) | p[1]) : 0;
        val = clamp16(((s32)(s16)pb->adpcm.gain * raw) >> 11);
        a->cur++;
        break;
    }
    case AX_SAMPLE_FORMAT_PCM_S8: {
        s32 raw = (s8)gx_read8(a->cur);
        val = clamp16((s32)(s16)pb->adpcm.gain * raw);
        a->cur++;
        break;
    }
    default:
        a->cur++;
        break;
    }

    // Read past the end address: back to the loop address. A looping voice
    // continues with the loop's ADPCM context (a stream keeps its history); a
    // one-shot stops, with the current address left on the loop address.
    if (a->cur == a->end + step - 1) {
        a->cur = a->start;
        if (pb->addr.loopFlag) {
            a->ps = pb->adpcmLoop.loop_pred_scale & 0x7F;
            if (pb->type != AX_VOICE_STREAM) {
                a->yn1 = (s16)pb->adpcmLoop.loop_yn1;
                a->yn2 = (s16)pb->adpcmLoop.loop_yn2;
            }
        } else {
            pb->state = AX_VOICE_STOP;
            a->stopped = TRUE;
        }
    }
    return (s16)val;
}

// Reads 96 output samples through the sample-rate converter (Dolphin's
// ResampleAudio; polyphase taps from sSrcCoef).
static void read_voice_samples(AXPB* pb, s16* out) {
    Accel a;
    a.pb = pb;
    a.start = hilo(pb->addr.loopAddressHi, pb->addr.loopAddressLo) & 0x3FFFFFFF;
    a.end = hilo(pb->addr.endAddressHi, pb->addr.endAddressLo) & 0x3FFFFFFF;
    a.cur = hilo(pb->addr.currentAddressHi, pb->addr.currentAddressLo) & 0xBFFFFFFF;
    a.yn1 = (s16)pb->adpcm.yn1;
    a.yn2 = (s16)pb->adpcm.yn2;
    a.ps = pb->adpcm.pred_scale;
    a.stopped = FALSE;

    u32 ratio = hilo(pb->src.ratioHi, pb->src.ratioLo);
    u32 pos = pb->src.currentAddressFrac;
    s16 t[4];
    u32 idx = 0;
    for (int i = 0; i < 4; i++) {
        t[idx++ & 3] = (s16)pb->src.last_samples[i];
    }

    if (pb->srcSelect == 0 || pb->srcSelect == 1) {
        const s32(*coef)[4] = pb->srcSelect == 0 ? sSrcCoef[pb->coefSelect > 2 ? 2 : pb->coefSelect] : NULL;
        for (int i = 0; i < NSAMP; i++) {
            pos += ratio;
            while (pos >= 0x10000) {
                t[idx++ & 3] = accel_read(&a);
                pos -= 0x10000;
            }
            // t[idx & 3] is the oldest of the last four input samples.
            s32 t0 = t[idx & 3], t1 = t[(idx + 1) & 3], t2 = t[(idx + 2) & 3], t3 = t[(idx + 3) & 3];
            if (coef != NULL) {
                const s32* c = coef[(pos & 0xFFFF) >> 9];
                out[i] = clamp16(((s64)t0 * c[0] + (s64)t1 * c[1] + (s64)t2 * c[2] + (s64)t3 * c[3]) >> 15);
            } else {
                u32 frac = pos & 0xFFFF;
                out[i] = frac ? (s16)(((s64)t0 * (0x10000 - frac) + (s64)t1 * frac) >> 16) : (s16)t0;
            }
        }
        for (int i = 0; i < 4; i++) {
            pb->src.last_samples[i] = (u16)t[(idx + i) & 3];
        }
    } else {
        // No conversion: one input sample per output sample.
        for (int i = 0; i < NSAMP; i++) {
            out[i] = accel_read(&a);
        }
        for (int i = 0; i < 4; i++) {
            pb->src.last_samples[i] = (u16)out[NSAMP - 4 + i];
        }
    }
    pb->src.currentAddressFrac = (u16)pos;

    pb->addr.currentAddressHi = (u16)(a.cur >> 16);
    pb->addr.currentAddressLo = (u16)a.cur;
    pb->adpcm.yn1 = (u16)a.yn1;
    pb->adpcm.yn2 = (u16)a.yn2;
    pb->adpcm.pred_scale = a.ps;
}

// 4-tap polyphase filters: interpolation between the 2nd and 3rd taps,
// Lanczos-windowed sinc with cutoffs of 8, 12 and 16 kHz at 32 kHz
// (0.25 / 0.375 / 0.5 cycles per input sample), each phase normalised to unity
// gain at DC.
static void init_src_coefs(void) {
    static const double kCutoff[3] = {0.25, 0.375, 0.5};
    for (int f = 0; f < 3; f++) {
        double fc = kCutoff[f];
        for (int p = 0; p < 128; p++) {
            double frac = p / 128.0;
            double h[4], sum = 0;
            for (int k = 0; k < 4; k++) {
                double d = (k - 1) - frac;
                double x = 2 * fc * d;
                double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
                double w = fabs(d) >= 2 ? 0.0 : fabs(d) < 1e-9 ? 1.0 : sin(M_PI * d / 2) / (M_PI * d / 2);
                h[k] = 2 * fc * sinc * w;
                sum += h[k];
            }
            for (int k = 0; k < 4; k++) {
                sSrcCoef[f][p][k] = (s32)lround(h[k] / sum * 32768.0);
            }
        }
    }
}

// ------------------------------------------------------------------ DSP: one voice (AXVoice.h ProcessVoice)

typedef struct BusDesc {
    u16 volOfs;   // offsetof(AXPBMIX, volume)
    u16 deltaOfs; // offsetof(AXPBMIX, delta)
    u16 dpopOfs;  // offsetof(AXPBDPOP, last sample)
    u32 onMask;   // mixerCtrl bits enabling the bus
    u32 rampMask; // mixerCtrl bit enabling the ramp
} BusDesc;

#define MIXF(f) (u16) offsetof(AXPBMIX, f)
#define DPOPF(f) (u16) offsetof(AXPBDPOP, f)
static const BusDesc kBus[BUS_COUNT] = {
    {MIXF(vL), MIXF(vDeltaL), DPOPF(aL), 0x1 | 0x4, 0x4},
    {MIXF(vR), MIXF(vDeltaR), DPOPF(aR), 0x2 | 0x4, 0x4},
    {MIXF(vS), MIXF(vDeltaS), DPOPF(aS), 0x8 | 0x10, 0x10},
    {MIXF(vAuxAL), MIXF(vDeltaAuxAL), DPOPF(aAuxAL), 0x10000 | 0x40000, 0x40000},
    {MIXF(vAuxAR), MIXF(vDeltaAuxAR), DPOPF(aAuxAR), 0x20000 | 0x40000, 0x40000},
    {MIXF(vAuxAS), MIXF(vDeltaAuxAS), DPOPF(aAuxAS), 0x80000 | 0x100000, 0x100000},
    {MIXF(vAuxBL), MIXF(vDeltaAuxBL), DPOPF(aAuxBL), 0x200000 | 0x800000, 0x800000},
    {MIXF(vAuxBR), MIXF(vDeltaAuxBR), DPOPF(aAuxBR), 0x400000 | 0x800000, 0x800000},
    {MIXF(vAuxBS), MIXF(vDeltaAuxBS), DPOPF(aAuxBS), 0x1000000 | 0x2000000, 0x2000000},
    {MIXF(vAuxCL), MIXF(vDeltaAuxCL), DPOPF(aAuxCL), 0x4000000 | 0x10000000, 0x10000000},
    {MIXF(vAuxCR), MIXF(vDeltaAuxCR), DPOPF(aAuxCR), 0x8000000 | 0x10000000, 0x10000000},
    {MIXF(vAuxCS), MIXF(vDeltaAuxCS), DPOPF(aAuxCS), 0x20000000 | 0x40000000, 0x40000000},
};

static void process_voice(AXPB* pb) {
    s16 smp[NSAMP];

    if (pb->state != AX_VOICE_RUN) {
        return;
    }

    read_voice_samples(pb, smp);

    // Volume envelope: unsigned volume (1.0 = 0x8000), signed per-sample delta.
    s32 vol = pb->ve.currentVolume;
    s32 dv = (s16)pb->ve.currentDelta;
    for (int i = 0; i < NSAMP; i++) {
        smp[i] = clamp16(((s64)smp[i] * vol) >> 15);
        vol += dv;
        vol = vol < 0 ? 0 : vol > 0xFFFF ? 0xFFFF : vol;
    }
    pb->ve.currentVolume = (u16)vol;
    pb->ve.currentDelta = 0; // one-frame ramps (see the top of the file)

    if (pb->lpf.on) {
        s32 y = (s16)pb->lpf.yn1;
        s32 a0 = pb->lpf.a0, b0 = pb->lpf.b0;
        for (int i = 0; i < NSAMP; i++) {
            y = clamp16(((s64)a0 * smp[i] + (s64)b0 * y) >> 15);
            smp[i] = (s16)y;
        }
        pb->lpf.yn1 = (u16)y;
    }

    if (pb->biquad.on) {
        AXPBBIQUAD* f = &pb->biquad;
        for (int i = 0; i < NSAMP; i++) {
            s16 x0 = smp[i];
            s64 tmp = (s64)(s16)f->b0 * x0 + (s64)(s16)f->b1 * (s16)f->xn1 + (s64)(s16)f->b2 * (s16)f->xn2 +
                      (s64)(s16)f->a1 * (s16)f->yn1 + (s64)(s16)f->a2 * (s16)f->yn2;
            tmp <<= 2;
            tmp += (tmp & 0x10000) ? 0x8000 : 0x7FFF;
            s16 y0 = clamp16(tmp >> 16);
            f->xn2 = f->xn1;
            f->yn2 = f->yn1;
            f->xn1 = (u16)x0;
            f->yn1 = (u16)y0;
            smp[i] = y0;
        }
    }

    u32 ctrl = pb->mixerCtrl;
    for (int b = 0; b < BUS_COUNT; b++) {
        const BusDesc* d = &kBus[b];
        if (!(ctrl & d->onMask)) {
            continue;
        }
        u16* pv = (u16*)((u8*)&pb->mix + d->volOfs);
        u16* pd = (u16*)((u8*)&pb->mix + d->deltaOfs);
        s32 v = *pv;
        s32 delta = (ctrl & d->rampMask) ? (s16)*pd : 0;
        s32* out = sBus[b];
        s16 last = 0;
        for (int i = 0; i < NSAMP; i++) {
            last = clamp16(((s64)smp[i] * v) >> 15);
            out[i] += last;
            v += delta;
            v = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
        }
        *pv = (u16)v;
        *pd = 0; // one-frame ramps
        *(s16*)((u8*)&pb->dpop + d->dpopOfs) = last;
    }
}

// ------------------------------------------------------------------ DSP: the frame (AXWii.cpp)

static void depop_init_bus(int b) {
    // __AXDepopFadeMain: start at the accumulated value, fall by at most 20
    // per sample.
    s32 all = sDepopAll[b];
    s32 avg = all / NSAMP;
    if (avg == 0) {
        sDepopAll[b] = 0;
        memset(sBus[b], 0, sizeof(sBus[b]));
        return;
    }
    avg = avg > 20 ? 20 : avg < -20 ? -20 : avg;
    for (int j = 0; j < NSAMP; j++) {
        sBus[b][j] = all - j * avg;
    }
    sDepopAll[b] = all - avg * NSAMP;
}

static void volume_ramp(u16* ramp, u16 from, u16 to) {
    for (int i = 0; i < NSAMP; i++) {
        ramp[i] = (u16)(from + ((s32)to - from) * (i + 1) / NSAMP);
    }
}

// Runs the aux callback on its buses and adds the result to `dst` with the
// return volume.
static void process_aux(int aux, int nchans, const int* srcBus, const int* dstBus) {
    void* chans[4];
    u16 ramp[NSAMP];
    for (int c = 0; c < nchans; c++) {
        chans[c] = sBus[srcBus[c]];
    }
    sAuxCallback[aux](chans, sAuxContext[aux]);
    volume_ramp(ramp, sLastAuxReturnVolume[aux], sAuxReturnVolume[aux]);
    sLastAuxReturnVolume[aux] = sAuxReturnVolume[aux];
    for (int c = 0; c < nchans; c++) {
        const s32* in = sBus[srcBus[c]];
        s32* out = sBus[dstBus[c]];
        for (int i = 0; i < NSAMP; i++) {
            out[i] += (s32)(((s64)in[i] * ramp[i]) >> 15);
        }
    }
}

static void dsp_frame(void) {
    for (int b = 0; b < BUS_COUNT; b++) {
        depop_init_bus(b);
    }

    for (int i = 0; i < AX_VOICE_MAX; i++) {
        process_voice(&sDspPB[i]);
    }

    if (sMode == AX_OUTPUT_DPL2) {
        // DPL2: the 4th channel of aux A/B travels in aux C R/S, and the right
        // surround of the main mix in aux C L; aux C has no effect of its own.
        static const int srcA[4] = {BUS_AL, BUS_AR, BUS_AS, BUS_CR};
        static const int srcB[4] = {BUS_BL, BUS_BR, BUS_BS, BUS_CS};
        static const int dst[4] = {BUS_L, BUS_R, BUS_S, BUS_CL};
        if (sAuxCallback[0] != NULL) {
            process_aux(0, 4, srcA, dst);
        }
        if (sAuxCallback[1] != NULL) {
            process_aux(1, 4, srcB, dst);
        }
    } else {
        static const int src[3][3] = {{BUS_AL, BUS_AR, BUS_AS}, {BUS_BL, BUS_BR, BUS_BS}, {BUS_CL, BUS_CR, BUS_CS}};
        static const int dst[3] = {BUS_L, BUS_R, BUS_S};
        for (int a = 0; a < 3; a++) {
            if (sAuxCallback[a] != NULL) {
                process_aux(a, 3, src[a], dst);
            }
        }
    }

    // Fold the surround channels into L/R. Stereo: AX adds S to both sides.
    // Surround and DPL2 have no stereo equivalent on the Wii (they are matrix
    // encoded for a decoder); a plain -3 dB downmix suits speakers and phones.
    if (sMode == AX_OUTPUT_STEREO) {
        for (int i = 0; i < NSAMP; i++) {
            sBus[BUS_L][i] += sBus[BUS_S][i];
            sBus[BUS_R][i] += sBus[BUS_S][i];
        }
    } else {
        const s32* ls = sBus[BUS_S];
        const s32* rs = sMode == AX_OUTPUT_DPL2 ? sBus[BUS_CL] : sBus[BUS_S];
        for (int i = 0; i < NSAMP; i++) {
            sBus[BUS_L][i] += (s32)(((s64)ls[i] * 23170) >> 15);
            sBus[BUS_R][i] += (s32)(((s64)rs[i] * 23170) >> 15);
        }
    }

    u16 ramp[NSAMP];
    volume_ramp(ramp, sLastMasterVolume, sMasterVolume);
    sLastMasterVolume = sMasterVolume;
    for (int i = 0; i < NSAMP; i++) {
        sOut[2 * i + 0] = clamp16(((s64)sBus[BUS_L][i] * ramp[i]) >> 15);
        sOut[2 * i + 1] = clamp16(((s64)sBus[BUS_R][i] * ramp[i]) >> 15);
    }
}

// The AI DMA callback (__AXOutAiCallback / __AXOutNewFrame). Interrupts are
// disabled.
static void ax_frame(void) {
    sync_pbs();
    service_callback_stack();
    dsp_frame();
    if (sUserCallback != NULL) {
        sUserCallback();
    }
    AIInitDMA(sOut, sizeof(sOut));
}

// ------------------------------------------------------------------ init (AX.c)

void AXInitEx(u32 mode) {
    (void)mode; // the output ring mode only changes the Wii's DMA buffering
    if (sInit) {
        return;
    }
    BOOL enabled = OSDisableInterrupts();

    init_src_coefs();

    sCallbackStack = NULL;
    for (int p = 0; p < STACK_MAX; p++) {
        sStackHead[p] = sStackTail[p] = NULL;
    }
    sMaxDspCycles = 243000000 / 667; // OS_BUS_CLOCK_SPEED / 667
    sRecDspCycles = 0;
    memset(sDspPB, 0, sizeof(sDspPB));
    memset(sVPB, 0, sizeof(sVPB));
    memset(sItd, 0, sizeof(sItd));
    for (u32 i = 0; i < AX_VOICE_MAX; i++) {
        AXVPB* vpb = &sVPB[i];
        vpb->index = i;
        vpb->itdBuffer = sItd[i];
        set_pb_default(vpb);
        vpb->priority = AX_PRIORITY_MIN;
        push_free_stack(vpb);
    }

    memset(sDepopAll, 0, sizeof(sDepopAll));
    for (int a = 0; a < 3; a++) {
        sAuxCallback[a] = NULL;
        sAuxContext[a] = NULL;
        sAuxReturnVolume[a] = sLastAuxReturnVolume[a] = AX_MAX_VOLUME;
    }
    sMode = AX_OUTPUT_STEREO;
    sMasterVolume = sLastMasterVolume = AX_MAX_VOLUME;
    sUserCallback = NULL;
    memset(sOut, 0, sizeof(sOut));

    sInit = TRUE;
    OSRestoreInterrupts(enabled);

    AIRegisterDMACallback(ax_frame);
    AIInitDMA(sOut, sizeof(sOut));
    AIStartDMA();
}

void AXInit(void) { AXInitEx(0); }
