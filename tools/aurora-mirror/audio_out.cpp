// Audio output for colosseum-standalone (see audio_out.h).
#include "audio_out.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

SDL_AudioStream* g_stream = nullptr;
uint32_t g_stream_rate = 0;
bool g_stream_started = false;
FILE* g_wav = nullptr;
uint32_t g_wav_rate = 0;
uint64_t g_wav_bytes = 0, g_wav_patched_bytes = 0;
uint64_t g_pushed = 0, g_dropped = 0;
std::vector<int16_t> g_buffer;  // pending stereo frames, LR
uint32_t g_buffer_rate = 0;
float g_gain = 1.0f;
bool g_gain_from_env = false;

// Latency: the device starts once kStartMs is queued, and the queue is
// steered toward kTargetMs; past kCapMs new samples are dropped (the emulation
// ran ahead of the device). Samples go out kFlushMs at a time.
constexpr uint32_t kStartMs = 40, kTargetMs = 60, kCapMs = 150, kFlushMs = 5;

void put_le(uint8_t* p, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i)
    p[i] = uint8_t(v >> (8 * i));
}

void wav_write_header() {
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  const uint32_t data = uint32_t(g_wav_bytes);
  put_le(h + 4, 36 + data, 4);
  put_le(h + 16, 16, 4);
  put_le(h + 20, 1, 2);  // PCM
  put_le(h + 22, 2, 2);  // stereo
  put_le(h + 24, g_wav_rate, 4);
  put_le(h + 28, g_wav_rate * 4, 4);
  put_le(h + 32, 4, 2);
  put_le(h + 34, 16, 2);
  std::memcpy(h + 36, "data", 4);
  put_le(h + 40, data, 4);
  std::fseek(g_wav, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), g_wav);
  std::fseek(g_wav, 0, SEEK_END);
  g_wav_patched_bytes = g_wav_bytes;
}

// Hands the frames gathered in g_buffer (at g_buffer_rate) to the dump and the
// device.
void flush() {
  const uint32_t rate = g_buffer_rate;
  const int bytes = int(g_buffer.size() * sizeof(int16_t));
  if (!bytes)
    return;
  if (g_wav) {
    if (!g_wav_rate) {
      g_wav_rate = rate;
      wav_write_header();
    } else if (rate != g_wav_rate) {
      static bool warned = false;
      if (!warned)
        std::printf("audio: DMA rate changed %u -> %u Hz; the dump keeps %u\n", g_wav_rate, rate, g_wav_rate);
      warned = true;
    }
    std::fwrite(g_buffer.data(), 1, size_t(bytes), g_wav);
    g_wav_bytes += uint64_t(bytes);
    if (g_wav_bytes - g_wav_patched_bytes >= uint64_t(g_wav_rate) * 4)  // keep it valid if killed
      wav_write_header();
  }
  if (g_stream) {
    if (rate != g_stream_rate) {
      const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
      SDL_SetAudioStreamFormat(g_stream, &spec, nullptr);
      g_stream_rate = rate;
    }
    const int bytes_per_ms = int(rate * 4 / 1000);
    const int queued = SDL_GetAudioStreamQueued(g_stream);
    if (queued > bytes_per_ms * int(kCapMs)) {
      g_dropped += uint64_t(bytes / 4);
    } else {
      SDL_PutAudioStreamData(g_stream, g_buffer.data(), bytes);
      if (!g_stream_started && queued + bytes >= bytes_per_ms * int(kStartMs)) {
        SDL_ResumeAudioStreamDevice(g_stream);
        g_stream_started = true;
      }
      // The guest makes samples by its clock and the device plays them by
      // its own: play up to 0.5% fast or slow to hold the queue near
      // kTargetMs, so it neither creeps into the cap nor runs dry.
      if (g_stream_started) {
        const int target = bytes_per_ms * int(kTargetMs);
        const float error = float(queued + bytes - target) / float(target);
        SDL_SetAudioStreamFrequencyRatio(g_stream, 1.0f + 0.005f * std::clamp(error, -1.0f, 1.0f));
      }
    }
  }
  g_buffer.clear();
}

}  // namespace

void audio_out_init(bool enabled) {
  if (const char* dump = std::getenv("STANDALONE_AUDIO_DUMP"); dump && *dump) {
    g_wav = std::fopen(dump, "wb");
    if (!g_wav)
      std::fprintf(stderr, "audio: cannot write %s\n", dump);
  }
  const char* enable = std::getenv("STANDALONE_AUDIO");
  if (enable && *enable)
    enabled = std::strcmp(enable, "0") != 0;
  if (!enabled)
    return;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    std::printf("audio: SDL audio unavailable: %s\n", SDL_GetError());
    return;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!g_stream) {
    std::printf("audio: no output device: %s\n", SDL_GetError());
    return;
  }
  g_stream_rate = spec.freq;
  if (const char* volume = std::getenv("STANDALONE_AUDIO_VOLUME"); volume && *volume) {
    g_gain = float(std::atof(volume) / 100.0);
    g_gain_from_env = true;
  }
  SDL_SetAudioStreamGain(g_stream, g_gain);
  std::printf("audio: output on %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_stream)));
}

void audio_out_push(const uint8_t* be_rl, size_t frames, uint32_t rate) {
  g_pushed += frames;
  if (rate != g_buffer_rate)
    flush();
  g_buffer_rate = rate;
  for (size_t i = 0; i < frames; ++i) {
    const uint8_t* p = be_rl + i * 4;
    g_buffer.push_back(int16_t(uint16_t(p[2] << 8 | p[3])));  // left
    g_buffer.push_back(int16_t(uint16_t(p[0] << 8 | p[1])));  // right
  }
  if (g_buffer.size() >= size_t(rate / 1000 * kFlushMs * 2))
    flush();
}

uint64_t audio_out_frames_pushed() { return g_pushed; }
uint64_t audio_out_frames_dropped() { return g_dropped; }
int audio_out_queued_ms() {
  return g_stream && g_stream_rate ? int(SDL_GetAudioStreamQueued(g_stream) * 1000ll / (g_stream_rate * 4)) : -1;
}

void audio_out_set_volume(float gain) {
  if (g_gain_from_env)
    return;
  g_gain = std::clamp(gain, 0.0f, 1.0f);
  if (g_stream)
    SDL_SetAudioStreamGain(g_stream, g_gain);
}

void audio_out_pause(bool paused) {
  if (!g_stream || !g_stream_started)
    return;
  if (paused) {
    flush();
    SDL_PauseAudioStreamDevice(g_stream);
  } else {
    SDL_ResumeAudioStreamDevice(g_stream);
  }
}

void audio_out_shutdown() {
  flush();
  if (g_wav) {
    if (g_wav_rate)
      wav_write_header();
    std::fclose(g_wav);
    g_wav = nullptr;
  }
  if (g_stream) {
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
  }
}
