// Audio output for colosseum-standalone: what the AI's audio DMA reads from
// guest RAM goes to an SDL3 audio stream (and optionally a WAV file).
//
// STANDALONE_AUDIO=0           no output device
// STANDALONE_AUDIO_DUMP=<wav>  also write every pushed sample to a WAV file
// STANDALONE_AUDIO_VOLUME=<0-100> output gain in percent
#pragma once

#include <cstddef>
#include <cstdint>

// enabled: whether to open an output device (STANDALONE_AUDIO=0 or 1 wins).
void audio_out_init(bool enabled = true);
// frames stereo frames of big-endian s16 in RL order (as the AI DMA reads
// them: Dolphin's Mixer::PushSamples), at rate Hz (32000 or 48000).
void audio_out_push(const uint8_t* be_rl, size_t frames, uint32_t rate);
uint64_t audio_out_frames_pushed();
uint64_t audio_out_frames_dropped();
int audio_out_queued_ms();  // what the output stream holds; -1 without one
// Output gain, 0..1 (STANDALONE_AUDIO_VOLUME, when set, wins at init).
void audio_out_set_volume(float gain);
// Paused, the device stops playing and keeps what it holds; resumed, it
// carries on from there.
void audio_out_pause(bool paused);
void audio_out_shutdown();
