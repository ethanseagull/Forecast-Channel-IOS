// Port-only audio controls (port/sdk/ai.c, port/sdk/ax.c).
//
// The Wii's AI ("audio interface") plays 32 kHz stereo that the DSP mixes in
// 3 ms frames of 96 samples. On the port, ai.c runs a frame clock thread: once
// per frame it calls the AI DMA callback (AX's frame function, or whatever
// replaced it) with "interrupts disabled", then queues the frame's 96 stereo
// samples on an SDL3 audio stream. Sample buffers handed to AIInitDMA are
// native-endian s16, interleaved L,R (the Wii's are big-endian).
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Before AIStartDMA (AXInit): no thread and no device; frames are produced
// only by port_audio_render. For tests and offline rendering.
void port_audio_set_offline(int offline);

// Offline mode: runs `frames` AX frames and writes 96 * frames stereo samples
// (L,R interleaved, 32 kHz, native s16) to `out` (NULL discards them).
void port_audio_render(int16_t* out, uint32_t frames);

// Frames produced since AIStartDMA.
uint64_t port_audio_frame_count(void);

// Frames queued in the audio stream as seen by the frame thread (lowest and
// highest), and how often it added frames (queue ran low: likely an underrun)
// or skipped one (device slower than the clock). Zeros/huge before a device.
void port_audio_stats(int* queuedMin, int* queuedMax, uint32_t* refills, uint32_t* skips);

// Stops the frame thread and closes the audio device. Call before SDL_Quit.
// Idempotent.
void port_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
