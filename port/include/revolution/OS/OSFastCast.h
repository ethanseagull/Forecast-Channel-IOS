// Port: OSFastCast without paired-single quantization registers.
// The GQRs are set to scale 0 by OSInitFastCast, so these are plain
// conversions; stores clamp to the target range like psq_st does.
#ifndef RVL_SDK_OS_FAST_CAST_H
#define RVL_SDK_OS_FAST_CAST_H
#include <types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OS_GQR_TYPE_U8 4
#define OS_GQR_TYPE_U16 5
#define OS_GQR_TYPE_S8 6
#define OS_GQR_TYPE_S16 7

static inline void OSInitFastCast(void) {}
// Only nw4r::g3d uses GQR6/7 (S10.5 / S7.8); its helpers hard-code those scales.
static inline void OSSetGQR6(u32 type, u32 scale) { (void)type; (void)scale; }
static inline void OSSetGQR7(u32 type, u32 scale) { (void)type; (void)scale; }

static inline f32 __OSclampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline f32 __OSu8tof32(u8* in) { return (f32)*in; }
static inline void OSu8tof32(u8* in, volatile f32* out) { *out = (f32)*in; }
static inline f32 __OSu16tof32(u16* in) { return (f32)*in; }
static inline void OSu16tof32(u16* in, volatile f32* out) { *out = (f32)*in; }
static inline f32 __OSs16tof32(s16* in) { return (f32)*in; }
static inline void OSs16tof32(s16* in, volatile f32* out) { *out = (f32)*in; }

static inline u8 __OSf32tou8(f32 v) { return (u8)__OSclampf(v, 0.0f, 255.0f); }
static inline void OSf32tou8(f32* in, volatile u8* out) { *out = __OSf32tou8(*in); }
static inline u16 __OSf32tou16(f32 v) { return (u16)__OSclampf(v, 0.0f, 65535.0f); }
static inline void OSf32tou16(f32* in, volatile u16* out) { *out = __OSf32tou16(*in); }
static inline s16 __OSf32tos16(f32 v) { return (s16)__OSclampf(v, -32768.0f, 32767.0f); }
static inline void OSf32tos16(f32* in, volatile s16* out) { *out = __OSf32tos16(*in); }

#ifdef __cplusplus
}
#endif
#endif
