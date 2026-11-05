// AX DSP high-level emulation: a port of Dolphin's AXUCode (GameCube AX),
// HLEAccelerator / DSP::Accelerator and the AXVoice.h mixer, with local shims
// for the Dolphin infrastructure they use. The mixing, resampling, ADPCM and
// command-list logic follows Dolphin line for line; see ax_hle.h for sources.
//
// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ax_hle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#define HILO_TO_32(name) ((u32(name##_hi) << 16) | name##_lo)

AxHostInterface g_host{};

// --- Shims: guest memory (Memory::MemoryManager) and ARAM ---------------------

u8* mem_ptr(u32 address, u32 length) {
  const u32 physical = address & 0x1FFFFFFF;
  if (!g_host.ram || physical >= g_host.ram_size || length > g_host.ram_size - physical)
    return nullptr;
  return g_host.ram + physical;
}

void warn_bad_address(u32 address) {
  static int warnings = 0;
  if (warnings < 16) {
    ++warnings;
    std::fprintf(stderr, "ax_hle: guest range at 0x%08X is outside MEM1\n", address);
  }
}

// CopyFromEmuSwapped<u16>
void copy_from_emu_u16(u16* dst, u32 address, size_t bytes) {
  const u8* p = mem_ptr(address, u32(bytes));
  if (!p) {
    warn_bad_address(address);
    std::memset(dst, 0, bytes);
    return;
  }
  for (size_t i = 0; i < bytes / 2; ++i)
    dst[i] = u16(p[2 * i] << 8 | p[2 * i + 1]);
}

// CopyToEmuSwapped<u16>
void copy_to_emu_u16(u32 address, const u16* src, size_t bytes) {
  u8* p = mem_ptr(address, u32(bytes));
  if (!p) {
    warn_bad_address(address);
    return;
  }
  for (size_t i = 0; i < bytes / 2; ++i) {
    p[2 * i] = u8(src[i] >> 8);
    p[2 * i + 1] = u8(src[i]);
  }
}

// CopyToEmuSwapped<int>
void copy_to_emu_s32(u32 address, const int* src, size_t bytes) {
  u8* p = mem_ptr(address, u32(bytes));
  if (!p) {
    warn_bad_address(address);
    return;
  }
  for (size_t i = 0; i < bytes / 4; ++i) {
    const u32 v = u32(src[i]);
    p[4 * i] = u8(v >> 24);
    p[4 * i + 1] = u8(v >> 16);
    p[4 * i + 2] = u8(v >> 8);
    p[4 * i + 3] = u8(v);
  }
}

// GetPointerForRange + Common::swap32 over <count> words.
void read_emu_s32(int* dst, u32 address, size_t count) {
  const u8* p = mem_ptr(address, u32(count * 4));
  if (!p) {
    warn_bad_address(address);
    std::memset(dst, 0, count * 4);
    return;
  }
  for (size_t i = 0; i < count; ++i)
    dst[i] = int(u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3]);
}

u8 read_aram(u32 address) {
  return g_host.aram[address & (g_host.aram_size - 1)];
}

void push_mail(u32 mail, bool interrupt) {
  if (std::getenv("STANDALONE_DSP_LOG")) {
    static int logged = 0;
    if (logged < 400) {
      ++logged;
      std::printf("  dsp mail -> 0x%08X%s\n", mail, interrupt ? " (interrupt)" : "");
    }
  }
  g_host.push_mail(mail, interrupt);
}

// --- AXStructs.h ---------------------------------------------------------------

struct VolumeData {
  u16 volume;
  u16 volume_delta;
};

struct PBMixer {
  VolumeData main_left, main_right;
  VolumeData auxA_left, auxA_right;
  VolumeData auxB_left, auxB_right;
  VolumeData auxB_surround;
  VolumeData main_surround;
  VolumeData auxA_surround;
};

struct PBInitialTimeDelay {
  u16 on;
  u16 addrMemHigh;
  u16 addrMemLow;
  u16 offsetLeft;
  u16 offsetRight;
  u16 targetLeft;
  u16 targetRight;
};

struct PBUpdate {
  u16 pb_offset;
  u16 new_value;
};
using PBUpdateData = std::array<PBUpdate, 32>;

struct PBUpdates {
  u16 num_updates[5];
  u16 data_hi;
  u16 data_lo;
};

struct PBDpop {
  s16 main_left;
  s16 auxA_left;
  s16 auxB_left;
  s16 main_right;
  s16 auxA_right;
  s16 auxB_right;
  s16 main_surround;
  s16 auxA_surround;
  s16 auxB_surround;
};

struct PBVolumeEnvelope {
  s16 cur_volume;
  s16 cur_volume_delta;
};

struct PBUnknown2 {
  u16 unknown_reserved[3];
};

struct PBAudioAddr {
  u16 looping;
  u16 sample_format;
  u16 loop_addr_hi;
  u16 loop_addr_lo;
  u16 end_addr_hi;
  u16 end_addr_lo;
  u16 cur_addr_hi;
  u16 cur_addr_lo;
};

struct PBADPCMInfo {
  s16 coefs[16];
  u16 gain;
  u16 pred_scale;
  s16 yn1;
  s16 yn2;
};

struct PBSampleRateConverter {
  u16 ratio_hi;
  u16 ratio_lo;
  u16 cur_addr_frac;
  s16 last_samples[4];
};

struct PBADPCMLoopInfo {
  u16 pred_scale;
  u16 yn1;
  u16 yn2;
};

struct PBLowPassFilter {
  u16 on;
  s16 yn1;
  u16 a0;
  s16 b0;
};

struct AXPB {
  u16 next_pb_hi;
  u16 next_pb_lo;
  u16 this_pb_hi;
  u16 this_pb_lo;

  u16 src_type;
  u16 coef_select;
  u16 mixer_control;

  u16 running;
  u16 is_stream;

  PBMixer mixer;
  PBInitialTimeDelay initial_time_delay;
  PBUpdates updates;
  PBDpop dpop;
  PBVolumeEnvelope vol_env;
  PBUnknown2 unknown3;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf;  // Skipped for ucode 0x4E8A8B21
  u16 loop_counter;

  u16 padding[24];
};
static_assert(sizeof(AXPB) % 2 == 0);

enum { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };

enum AXMixControl : u32 {
  MIX_MAIN_L = 0x000001,
  MIX_MAIN_L_RAMP = 0x000002,
  MIX_MAIN_R = 0x000004,
  MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010,
  MIX_MAIN_S_RAMP = 0x000020,
  MIX_AUXA_L = 0x000040,
  MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100,
  MIX_AUXA_R_RAMP = 0x000200,
  MIX_AUXA_S = 0x000400,
  MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000,
  MIX_AUXB_L_RAMP = 0x002000,
  MIX_AUXB_R = 0x004000,
  MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000,
  MIX_AUXB_S_RAMP = 0x020000,
  MIX_ALL_RAMPS = 0xAAAAAA,
};

// --- DSPAccelerator.{h,cpp} + HLEAccelerator (AXVoice.h) -----------------------

class HLEAccelerator {
public:
  AXPB* acc_pb = nullptr;

  u16 ReadSample(const s16* coefs);

  u32 GetCurrentAddress() const { return m_current_address; }
  s16 GetYn1() const { return m_yn1; }
  s16 GetYn2() const { return m_yn2; }
  u16 GetPredScale() const { return m_pred_scale; }
  void SetStartAddress(u32 address) { m_start_address = address & 0x3fffffff; }
  void SetEndAddress(u32 address) { m_end_address = address & 0x3fffffff; }
  void SetCurrentAddress(u32 address) { m_current_address = address & 0xbfffffff; }
  void SetSampleFormat(u16 format) { m_sample_format = format; }
  void SetGain(s16 gain) { m_gain = gain; }
  void SetYn1(s16 yn1) { m_yn1 = yn1; }
  void SetYn2(s16 yn2) {
    m_yn2 = yn2;
    m_reads_stopped = false;
  }
  void SetPredScale(u16 pred_scale) { m_pred_scale = pred_scale & 0x7f; }

private:
  // SampleFormat bitfields: size [0,2), decode [2,4), gain_scale [4,6), unk [6,16).
  u16 format_size() const { return m_sample_format & 3; }
  u16 format_decode() const { return (m_sample_format >> 2) & 3; }
  u16 format_gain_scale() const { return (m_sample_format >> 4) & 3; }

  enum { Size4Bit = 0, Size8Bit = 1, Size16Bit = 2 };
  enum { ADPCM = 0, MMIOPCMNoInc = 1, PCM = 2, MMIOPCMInc = 3 };
  enum { GainScale2048 = 0, GainScale1 = 1, GainScale65536 = 2 };

  u16 GetCurrentSample();
  void OnSampleReadEndException();
  u8 ReadMemory(u32 address) { return read_aram(address); }

  u32 m_start_address = 0;
  u32 m_end_address = 0;
  u32 m_current_address = 0;
  u16 m_sample_format = 0;
  s16 m_gain = 0;
  s16 m_yn1 = 0;
  s16 m_yn2 = 0;
  u16 m_pred_scale = 0;
  u16 m_input = 0;
  bool m_reads_stopped = false;
};

u16 HLEAccelerator::GetCurrentSample() {
  u16 val = 0;
  switch (format_size()) {
  case Size4Bit:
    val = ReadMemory(m_current_address >> 1);
    if (m_current_address & 1)
      val &= 0xf;
    else
      val >>= 4;
    break;
  case Size8Bit:
    val = ReadMemory(m_current_address);
    break;
  case Size16Bit:
    val = u16((ReadMemory(m_current_address * 2) << 8) | ReadMemory(m_current_address * 2 + 1));
    break;
  default:  // produces garbage, but affects the current address
    break;
  }
  return val;
}

void HLEAccelerator::OnSampleReadEndException() {
  if (acc_pb->audio_addr.looping) {
    // Set the ADPCM info to continue processing at loop_addr.
    SetPredScale(acc_pb->adpcm_loop_info.pred_scale);
    if (acc_pb->is_stream != 1) {
      SetYn1(s16(acc_pb->adpcm_loop_info.yn1));
      SetYn2(s16(acc_pb->adpcm_loop_info.yn2));
    } else {
      // Refresh YN1 and YN2. This indirectly causes the accelerator to resume reads.
      SetYn1(GetYn1());
      SetYn2(GetYn2());
      // If we're streaming, increment the loop counter.
      acc_pb->loop_counter++;
    }
  } else {
    // Non looping voice reached the end -> running = 0.
    acc_pb->running = 0;
  }
}

u16 HLEAccelerator::ReadSample(const s16* coefs) {
  if (m_reads_stopped)
    return 0x0000;

  u16 val = 0;
  u8 step_size = 0;
  s16 raw_sample;
  const u16 decode = format_decode();
  if (decode == MMIOPCMNoInc || decode == MMIOPCMInc)
    raw_sample = s16(m_input);
  else
    raw_sample = s16(GetCurrentSample());

  const int coef_idx = (m_pred_scale >> 4) & 0x7;
  const s32 coef1 = coefs[coef_idx * 2 + 0];
  const s32 coef2 = coefs[coef_idx * 2 + 1];

  switch (decode) {
  case ADPCM: {
    raw_sample &= 0xF;
    const int scale = 1 << (m_pred_scale & 0xF);
    if (raw_sample >= 8)
      raw_sample -= 16;

    const s32 val32 = (scale * raw_sample) + ((0x400 + coef1 * m_yn1 + coef2 * m_yn2) >> 11);
    val = u16(static_cast<s16>(std::clamp<s32>(val32, -0x7FFF, 0x7FFF)));
    step_size = 2;

    m_yn2 = m_yn1;
    m_yn1 = s16(val);
    m_current_address += 1;

    if ((m_end_address & 0xf) == 0x0 && m_current_address == m_end_address) {
      m_current_address = m_start_address + 1;
    } else if ((m_end_address & 0xf) == 0x1 && m_current_address == m_end_address - 1) {
      m_current_address = m_start_address;
    } else if ((m_current_address & 15) == 0) {
      m_pred_scale = ReadMemory((m_current_address & ~15u) >> 1);
      m_current_address += 2;
      step_size += 2;
    }
    break;
  }
  case MMIOPCMNoInc:
  case PCM:
  case MMIOPCMInc: {
    u8 gain_shift = 0;
    switch (format_gain_scale()) {
    case GainScale2048: gain_shift = 11; break;
    case GainScale1: gain_shift = 0; break;
    case GainScale65536: gain_shift = 16; break;
    default: break;
    }
    const s32 val32 = ((static_cast<s32>(m_gain) * raw_sample) >> gain_shift) +
                      (((coef1 * m_yn1) >> gain_shift) + ((coef2 * m_yn2) >> gain_shift));
    val = u16(static_cast<s16>(val32));
    m_yn2 = m_yn1;
    m_yn1 = s16(val);
    step_size = 2;
    if (decode != MMIOPCMNoInc)
      m_current_address += 1;
    break;
  }
  }

  // Check for loop.
  if (m_current_address == (m_end_address + step_size - 1)) {
    m_current_address = m_start_address;
    m_reads_stopped = true;
    OnSampleReadEndException();
  }

  SetCurrentAddress(m_current_address);
  return val;
}

// --- AXVoice.h (AX_GC) -----------------------------------------------------------

constexpr u32 MAX_SAMPLES_PER_FRAME = 32;

PBUpdateData LoadPBUpdates(const AXPB& pb) {
  PBUpdateData updates;
  const u32 updates_addr = HILO_TO_32(pb.updates.data);
  copy_from_emu_u16(reinterpret_cast<u16*>(updates.data()), updates_addr, sizeof(updates));
  return updates;
}

void ApplyUpdatesForMs(int curr_ms, AXPB& pb, const u16* num_updates, const PBUpdateData& updates) {
  std::array<u16, sizeof(AXPB) / 2> pb_mem;
  std::memcpy(pb_mem.data(), &pb, sizeof(AXPB));

  u32 start_idx = 0;
  for (int i = 0; i < curr_ms; ++i)
    start_idx += num_updates[i];

  for (u32 i = start_idx; i < start_idx + num_updates[curr_ms]; ++i) {
    // Bounds checks: Dolphin indexes these unchecked; out-of-range data from
    // the guest is ignored here instead of corrupting host memory.
    if (i >= updates.size())
      break;
    const u16 update_off = updates[i].pb_offset;
    const u16 update_val = updates[i].new_value;
    if (update_off < pb_mem.size())
      pb_mem[update_off] = update_val;
  }

  std::memcpy(&pb, pb_mem.data(), sizeof(AXPB));
}

struct AXBuffers {
  int* main_left;
  int* main_right;
  int* main_surround;
  int* auxA_left;
  int* auxA_right;
  int* auxA_surround;
  int* auxB_left;
  int* auxB_right;
  int* auxB_surround;
  int** ptrs() { return &main_left; }
};

void AcceleratorSetup(HLEAccelerator* accelerator, AXPB* pb) {
  accelerator->acc_pb = pb;
  accelerator->SetStartAddress(HILO_TO_32(pb->audio_addr.loop_addr));
  accelerator->SetEndAddress(HILO_TO_32(pb->audio_addr.end_addr));
  accelerator->SetCurrentAddress(HILO_TO_32(pb->audio_addr.cur_addr));
  accelerator->SetSampleFormat(pb->audio_addr.sample_format);
  accelerator->SetYn1(pb->adpcm.yn1);
  accelerator->SetYn2(pb->adpcm.yn2);
  accelerator->SetGain(s16(pb->adpcm.gain));
  accelerator->SetPredScale(pb->adpcm.pred_scale);
}

u16 AcceleratorGetSample(HLEAccelerator* accelerator) {
  return accelerator->ReadSample(accelerator->acc_pb->adpcm.coefs);
}

s16 ClampS16(s64 sample) {
  return s16(std::clamp<s64>(sample, -0x8000, 0x7FFF));
}

template <typename Input>
u32 ResampleAudio(Input input_callback, s16* output, u32 count, s16* last_samples, u32 curr_pos,
                  u32 ratio, int srctype, const s16* coeffs) {
  int read_samples_count = 0;

  if (coeffs && srctype == SRCTYPE_POLYPHASE) {
    s16 temp[4];
    u32 idx = 0;

    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];

    for (u32 i = 0; i < count; ++i) {
      curr_pos += ratio;
      while (curr_pos >= 0x10000) {
        temp[idx++ & 3] = input_callback(read_samples_count++);
        curr_pos -= 0x10000;
      }

      const u16 curr_pos_frac = u16(((curr_pos & 0xFFFF) >> 9) << 2);
      const s16* c = &coeffs[curr_pos_frac];

      const s64 t0 = temp[idx++ & 3];
      const s64 t1 = temp[idx++ & 3];
      const s64 t2 = temp[idx++ & 3];
      const s64 t3 = temp[idx++ & 3];

      const s64 samp = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;
      output[i] = ClampS16(samp);  // MathUtil::SaturatingCast<s16>
    }

    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  } else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
    s16 temp[4];
    u32 idx = 0;

    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];

    for (u32 i = 0; i < count; ++i) {
      curr_pos += ratio;

      while (curr_pos >= 0x10000) {
        temp[idx++ & 3] = input_callback(read_samples_count++);
        curr_pos -= 0x10000;
      }

      const u16 curr_frac = u16(curr_pos & 0xFFFF);
      const u16 inv_curr_frac = u16(-curr_frac);

      s16 sample;
      if (curr_frac) {
        const s32 s0 = temp[idx++ & 3];
        const s32 s1 = temp[idx++ & 3];

        sample = s16(((s0 * inv_curr_frac) + (s1 * curr_frac)) >> 16);
        idx += 2;
      } else {
        sample = temp[idx++ & 3];
        idx += 3;
      }

      output[i] = sample;
    }

    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  } else {  // SRCTYPE_NEAREST
    for (u32 i = 0; i < count; ++i)
      output[i] = s16(input_callback(i));

    std::memcpy(last_samples, output + count - 4, 4 * sizeof(u16));
  }

  return curr_pos;
}

void GetInputSamples(HLEAccelerator* accelerator, AXPB& pb, s16* samples, u16 count, const s16* coeffs) {
  AcceleratorSetup(accelerator, &pb);

  if (coeffs)
    coeffs += pb.coef_select * 0x200;
  const u32 curr_pos =
      ResampleAudio([accelerator](u32) { return s16(AcceleratorGetSample(accelerator)); }, samples, count,
                    pb.src.last_samples, pb.src.cur_addr_frac, HILO_TO_32(pb.src.ratio), pb.src_type, coeffs);
  pb.src.cur_addr_frac = u16(curr_pos & 0xFFFF);

  pb.audio_addr.cur_addr_hi = static_cast<u16>(accelerator->GetCurrentAddress() >> 16);
  pb.audio_addr.cur_addr_lo = static_cast<u16>(accelerator->GetCurrentAddress());
  pb.adpcm.yn1 = accelerator->GetYn1();
  pb.adpcm.yn2 = accelerator->GetYn2();
  pb.adpcm.pred_scale = accelerator->GetPredScale();
}

void MixAdd(int* out, const s16* input, u32 count, VolumeData* vd, s16* dpop, bool ramp) {
  u16& volume = vd->volume;
  u16 volume_delta = vd->volume_delta;

  if (!ramp)
    volume_delta = 0;

  for (u32 i = 0; i < count; ++i) {
    s64 sample = input[i];
    sample *= volume;
    sample >>= 15;
    const s16 sample16 = ClampS16(s32(sample));

    out[i] += sample16;
    volume += volume_delta;

    *dpop = sample16;
  }
}

void LowPassFilter(s16* samples, u32 count, PBLowPassFilter& f) {
  for (u32 i = 0; i < count; ++i)
    f.yn1 = samples[i] = ClampS16((f.a0 * s32(samples[i]) + f.b0 * s32(f.yn1)) >> 15);
}

void ProcessVoice(HLEAccelerator* accelerator, AXPB& pb, const AXBuffers& buffers, u16 count, u32 mctrl,
                  const s16* coeffs) {
  if (pb.running != 1)
    return;

  s16 samples[MAX_SAMPLES_PER_FRAME];
  GetInputSamples(accelerator, pb, samples, count, coeffs);

  for (u32 i = 0; i < count; ++i) {
    const s32 volume = s16(pb.vol_env.cur_volume);  // signed on GameCube
    const s32 sample = (s32(samples[i]) * volume) >> 15;
    samples[i] = ClampS16(sample);
    pb.vol_env.cur_volume = s16(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
  }

  if (pb.lpf.on != 0)
    LowPassFilter(samples, count, pb.lpf);

#define MIX_ON(C) (0 != (mctrl & MIX_##C))
#define RAMP_ON(C) (0 != (mctrl & MIX_##C##_RAMP))

  if (MIX_ON(MAIN_L))
    MixAdd(buffers.main_left, samples, count, &pb.mixer.main_left, &pb.dpop.main_left, RAMP_ON(MAIN_L));
  if (MIX_ON(MAIN_R))
    MixAdd(buffers.main_right, samples, count, &pb.mixer.main_right, &pb.dpop.main_right, RAMP_ON(MAIN_R));
  if (MIX_ON(MAIN_S))
    MixAdd(buffers.main_surround, samples, count, &pb.mixer.main_surround, &pb.dpop.main_surround,
           RAMP_ON(MAIN_S));

  if (MIX_ON(AUXA_L))
    MixAdd(buffers.auxA_left, samples, count, &pb.mixer.auxA_left, &pb.dpop.auxA_left, RAMP_ON(AUXA_L));
  if (MIX_ON(AUXA_R))
    MixAdd(buffers.auxA_right, samples, count, &pb.mixer.auxA_right, &pb.dpop.auxA_right, RAMP_ON(AUXA_R));
  if (MIX_ON(AUXA_S))
    MixAdd(buffers.auxA_surround, samples, count, &pb.mixer.auxA_surround, &pb.dpop.auxA_surround,
           RAMP_ON(AUXA_S));

  if (MIX_ON(AUXB_L))
    MixAdd(buffers.auxB_left, samples, count, &pb.mixer.auxB_left, &pb.dpop.auxB_left, RAMP_ON(AUXB_L));
  if (MIX_ON(AUXB_R))
    MixAdd(buffers.auxB_right, samples, count, &pb.mixer.auxB_right, &pb.dpop.auxB_right, RAMP_ON(AUXB_R));
  if (MIX_ON(AUXB_S))
    MixAdd(buffers.auxB_surround, samples, count, &pb.mixer.auxB_surround, &pb.dpop.auxB_surround,
           RAMP_ON(AUXB_S));

#undef MIX_ON
#undef RAMP_ON
  // initial_time_delay (ITD) is unimplemented in Dolphin as well.
}

// --- UCodes.h task mails ---------------------------------------------------------

constexpr u32 TASK_MAIL_MASK = 0xFFFF'0000;
constexpr u32 TASK_MAIL_TO_CPU = 0xDCD1'0000;
constexpr u32 DSP_INIT = TASK_MAIL_TO_CPU | 0x0000;
constexpr u32 DSP_RESUME = TASK_MAIL_TO_CPU | 0x0001;
constexpr u32 DSP_YIELD = TASK_MAIL_TO_CPU | 0x0002;
constexpr u32 DSP_DONE = TASK_MAIL_TO_CPU | 0x0003;
constexpr u32 TASK_MAIL_TO_DSP = 0xCDD1'0000;
constexpr u32 MAIL_RESUME = TASK_MAIL_TO_DSP | 0x0000;
constexpr u32 MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 0x0001;
constexpr u32 MAIL_RESET = TASK_MAIL_TO_DSP | 0x0002;
constexpr u32 MAIL_CONTINUE = TASK_MAIL_TO_DSP | 0x0003;
constexpr u32 CARD_UCODE_CRC = 0x65d6cc6f;

// --- AX.h / AX.cpp ---------------------------------------------------------------

class AXUCode {
public:
  void Initialize(u32 crc);
  void HandleMail(u32 mail);
  void Update();

  // Which DSP program runs: this AX task, Dolphin's CARD HLE (a ucode switch
  // away from AX), or the ROM (after MAIL_RESET or the CARD task's end).
  enum class Mode { AX, Card, Rom };
  Mode mode = Mode::Rom;
  u32 m_crc = 0;
  bool saved_ax = false;  // AX state kept across a switch (DSPHLE's m_last_ucode)
  u64 command_lists = 0;

private:
  static constexpr u32 MAIL_CMDLIST = 0xBABE0000;
  static constexpr u32 MAIL_CMDLIST_MASK = 0xFFFF0000;

  int m_samples_main_left[32 * 5]{};
  int m_samples_main_right[32 * 5]{};
  int m_samples_main_surround[32 * 5]{};
  int m_samples_auxA_left[32 * 5]{};
  int m_samples_auxA_right[32 * 5]{};
  int m_samples_auxA_surround[32 * 5]{};
  int m_samples_auxB_left[32 * 5]{};
  int m_samples_auxB_right[32 * 5]{};
  int m_samples_auxB_surround[32 * 5]{};

  // Dolphin keeps 512 words; the slack keeps a list without CMD_END from
  // reading past the buffer.
  static constexpr u32 kCmdListWords = 512;
  u16 m_cmdlist[kCmdListWords + 16]{};
  u32 m_cmdlist_size = 0;

  // Polyphase coefficients come from the DSP DROM (dsp_coef.bin in Dolphin).
  // Without that file Dolphin passes nullptr and ProcessVoice falls back to
  // linear interpolation for SRCTYPE_POLYPHASE voices; this port always does.
  bool m_coeffs_valid = false;
  std::array<s16, 0x800> m_coeffs{};

  u16 m_compressor_pos = 0;

  HLEAccelerator m_accelerator;

  // UCodeInterface: PrepareBootUCode state.
  bool m_upload_setup_in_progress = false;
  bool m_needs_resume_mail = false;
  int m_next_ucode_steps = 0;
  u32 m_next_iram_mram_addr = 0;
  u16 m_next_iram_size = 0;

  enum CmdType {
    CMD_SETUP = 0x00,
    CMD_DL_AND_VOL_MIX = 0x01,
    CMD_PB_ADDR = 0x02,
    CMD_PROCESS = 0x03,
    CMD_MIX_AUXA = 0x04,
    CMD_MIX_AUXB = 0x05,
    CMD_UPLOAD_LRS = 0x06,
    CMD_SET_LR = 0x07,
    CMD_UNK_08 = 0x08,
    CMD_MIX_AUXB_NOWRITE = 0x09,
    CMD_UNK_0A = 0x0A,
    CMD_UNK_0B = 0x0B,
    CMD_UNK_0C = 0x0C,
    CMD_MORE = 0x0D,
    CMD_OUTPUT = 0x0E,
    CMD_END = 0x0F,
    CMD_MIX_AUXB_LR = 0x10,
    CMD_SET_OPPOSITE_LR = 0x11,
    CMD_COMPRESSOR = 0x12,
    CMD_SEND_AUX_AND_MIX = 0x13,
  };

  enum class MailState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };
  MailState m_mail_state = MailState::WaitingForCmdListSize;

  void ResetState();
  void PrepareBootUCode(u32 mail);
  void SwapUCode(u32 crc);
  void CopyCmdList(u32 addr, u16 size);
  u32 ConvertMixerControl(u32 mixer_control) const;
  void HandleCommandList();
  void SignalWorkEnd();

  struct BufferDesc {
    int* ptr;
    int samples_per_milli;
  };
  void SetupProcessing(u32 init_addr);
  void DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb);
  void ReadPB(u32 addr, AXPB& pb) const;
  void WritePB(u32 addr, const AXPB& pb) const;
  void ProcessPBList(u32 pb_addr);
  void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr);
  void UploadLRS(u32 dst_addr);
  void SetMainLR(u32 src_addr);
  void RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis);
  void OutputSamples(u32 lr_addr, u32 surround_addr);
  void MixAUXBLR(u32 ul_addr, u32 dl_addr);
  void SetOppositeLR(u32 src_addr);
  void SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                     u32 auxb_r_dl);
};

AXUCode g_ax;

void AXUCode::ResetState() {
  const u32 crc = m_crc;
  const bool saved = saved_ax;
  const u64 lists = command_lists;
  this->~AXUCode();
  new (this) AXUCode();
  m_crc = crc;
  saved_ax = saved;
  command_lists = lists;
}

void AXUCode::Initialize(u32 crc) {
  if (saved_ax && crc == m_crc) {
    // DSPHLE::SwapUCode back to the kept AX ucode: no new DSP_INIT; the resume
    // mail flagged by PrepareBootUCode goes out on the next Update.
    saved_ax = false;
    mode = Mode::AX;
    return;
  }
  m_crc = crc;
  saved_ax = false;
  ResetState();
  mode = Mode::AX;
  // InitializeShared
  push_mail(DSP_INIT, true);
  // LoadResamplingCoefficients(false, 0): no dsp_coef.bin here, so
  // m_coeffs_checksum stays empty and polyphase voices resample linearly.
  m_coeffs_valid = false;
}

void AXUCode::SignalWorkEnd() {
  // Dolphin delays this interrupt by AX_EMPTY_COMMAND_LIST_CYCLES (2500); the
  // host raises it at once.
  push_mail(DSP_YIELD, true);
}

void AXUCode::HandleCommandList() {
  u16 addr_hi, addr_lo;
  u16 addr2_hi, addr2_lo;
  u16 size;

  u32 pb_addr = 0;

  u32 curr_idx = 0;
  bool end = false;
  ++command_lists;
  while (!end) {
    if (curr_idx >= kCmdListWords) {
      std::fprintf(stderr, "ax_hle: command list overran its buffer\n");
      break;
    }
    const u16 cmd = m_cmdlist[curr_idx++];

    switch (cmd) {
    case CMD_SETUP:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      SetupProcessing(HILO_TO_32(addr));
      break;

    case CMD_DL_AND_VOL_MIX: {
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      const u16 vol_main = m_cmdlist[curr_idx++];
      const u16 vol_auxa = m_cmdlist[curr_idx++];
      const u16 vol_auxb = m_cmdlist[curr_idx++];
      DownloadAndMixWithVolume(HILO_TO_32(addr), vol_main, vol_auxa, vol_auxb);
      break;
    }

    case CMD_PB_ADDR:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      pb_addr = HILO_TO_32(addr);
      break;

    case CMD_PROCESS:
      ProcessPBList(pb_addr);
      break;

    case CMD_MIX_AUXA:
    case CMD_MIX_AUXB:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      addr2_hi = m_cmdlist[curr_idx++];
      addr2_lo = m_cmdlist[curr_idx++];
      MixAUXSamples(cmd - CMD_MIX_AUXA, HILO_TO_32(addr), HILO_TO_32(addr2));
      break;

    case CMD_UPLOAD_LRS:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      UploadLRS(HILO_TO_32(addr));
      break;

    case CMD_SET_LR:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      SetMainLR(HILO_TO_32(addr));
      break;

    case CMD_UNK_08:
      curr_idx += 10;
      break;

    case CMD_MIX_AUXB_NOWRITE:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      MixAUXSamples(1, 0, HILO_TO_32(addr));
      break;

    case CMD_UNK_0A:
    case CMD_UNK_0B:
    case CMD_UNK_0C:
      break;

    case CMD_MORE:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      size = m_cmdlist[curr_idx++];
      CopyCmdList(HILO_TO_32(addr), size);
      curr_idx = 0;
      break;

    case CMD_OUTPUT:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      addr2_hi = m_cmdlist[curr_idx++];
      addr2_lo = m_cmdlist[curr_idx++];
      OutputSamples(HILO_TO_32(addr2), HILO_TO_32(addr));
      break;

    case CMD_END:
      end = true;
      break;

    case CMD_MIX_AUXB_LR:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      addr2_hi = m_cmdlist[curr_idx++];
      addr2_lo = m_cmdlist[curr_idx++];
      MixAUXBLR(HILO_TO_32(addr), HILO_TO_32(addr2));
      break;

    case CMD_SET_OPPOSITE_LR:
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      SetOppositeLR(HILO_TO_32(addr));
      break;

    case CMD_COMPRESSOR: {
      const u16 threshold = m_cmdlist[curr_idx++];
      const u16 frames = m_cmdlist[curr_idx++];
      addr_hi = m_cmdlist[curr_idx++];
      addr_lo = m_cmdlist[curr_idx++];
      RunCompressor(threshold, frames, HILO_TO_32(addr), 5);
      break;
    }

    case CMD_SEND_AUX_AND_MIX: {
      const u16 auxa_lrs_up_hi = m_cmdlist[curr_idx++];
      const u16 auxa_lrs_up_lo = m_cmdlist[curr_idx++];
      const u16 auxb_s_up_hi = m_cmdlist[curr_idx++];
      const u16 auxb_s_up_lo = m_cmdlist[curr_idx++];
      const u16 main_l_dl_hi = m_cmdlist[curr_idx++];
      const u16 main_l_dl_lo = m_cmdlist[curr_idx++];
      const u16 main_r_dl_hi = m_cmdlist[curr_idx++];
      const u16 main_r_dl_lo = m_cmdlist[curr_idx++];
      const u16 auxb_l_dl_hi = m_cmdlist[curr_idx++];
      const u16 auxb_l_dl_lo = m_cmdlist[curr_idx++];
      const u16 auxb_r_dl_hi = m_cmdlist[curr_idx++];
      const u16 auxb_r_dl_lo = m_cmdlist[curr_idx++];
      SendAUXAndMix(HILO_TO_32(auxa_lrs_up), HILO_TO_32(auxb_s_up), HILO_TO_32(main_l_dl),
                    HILO_TO_32(main_r_dl), HILO_TO_32(auxb_l_dl), HILO_TO_32(auxb_r_dl));
      break;
    }

    default: {
      static int warnings = 0;
      if (warnings < 16) {
        ++warnings;
        std::fprintf(stderr, "ax_hle: unknown command in AX command list: %04x\n", cmd);
      }
      end = true;
      break;
    }
    }
  }
}

u32 AXUCode::ConvertMixerControl(u32 mixer_control) const {
  u32 ret = 0;

  if (m_crc == 0x4e8a8b21) {
    if (mixer_control & 0x0010) {
      // DPL2 mixing
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if ((mixer_control & 0x0006) == 0)
        ret |= MIX_AUXB_L | MIX_AUXB_R;
      if ((mixer_control & 0x0007) == 1)
        ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
    } else {
      // non-DPL2 mixing
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if (mixer_control & 0x0001)
        ret |= MIX_AUXA_L | MIX_AUXA_R;
      if (mixer_control & 0x0002)
        ret |= MIX_AUXB_L | MIX_AUXB_R;
      if (mixer_control & 0x0004) {
        ret |= MIX_MAIN_S;
        if (ret & MIX_AUXA_L)
          ret |= MIX_AUXA_S;
        if (ret & MIX_AUXB_L)
          ret |= MIX_AUXB_S;
      }
    }
    if (mixer_control & 0x0008)
      ret |= MIX_ALL_RAMPS;
  } else {
    // newer GameCube ucodes
    if (mixer_control & 0x0001)
      ret |= MIX_MAIN_L;
    if (mixer_control & 0x0002)
      ret |= MIX_MAIN_R;
    if (mixer_control & 0x0004)
      ret |= MIX_MAIN_S;
    if (mixer_control & 0x0008)
      ret |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;

    if (mixer_control & 0x0010)
      ret |= MIX_AUXA_L;
    if (mixer_control & 0x0020)
      ret |= MIX_AUXA_R;
    if (mixer_control & 0x0040)
      ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mixer_control & 0x0080)
      ret |= MIX_AUXA_S;
    if (mixer_control & 0x0100)
      ret |= MIX_AUXA_S_RAMP;

    if (mixer_control & 0x0200)
      ret |= MIX_AUXB_L;
    if (mixer_control & 0x0400)
      ret |= MIX_AUXB_R;
    if (mixer_control & 0x0800)
      ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mixer_control & 0x1000)
      ret |= MIX_AUXB_S;
    if (mixer_control & 0x2000)
      ret |= MIX_AUXB_S_RAMP;
  }

  return ret;
}

void AXUCode::SetupProcessing(u32 init_addr) {
  const std::array<BufferDesc, 9> buffers = {{
      {m_samples_main_left, 32},
      {m_samples_main_right, 32},
      {m_samples_main_surround, 32},
      {m_samples_auxA_left, 32},
      {m_samples_auxA_right, 32},
      {m_samples_auxA_surround, 32},
      {m_samples_auxB_left, 32},
      {m_samples_auxB_right, 32},
      {m_samples_auxB_surround, 32},
  }};
  // InitMixingBuffers<5 /*ms*/>
  constexpr int Millis = 5;
  std::array<u16, 3 * 9> init_array;
  copy_from_emu_u16(init_array.data(), init_addr, sizeof(init_array));
  for (size_t i = 0; i < buffers.size(); ++i) {
    const BufferDesc& buf = buffers[i];
    const s32 value = s32((u32(init_array[3 * i]) << 16) | init_array[3 * i + 1]);
    const s16 delta = s16(init_array[3 * i + 2]);
    if (value == 0) {
      std::memset(buf.ptr, 0, Millis * buf.samples_per_milli * sizeof(int));
    } else {
      for (int j = 0; j < Millis * buf.samples_per_milli; ++j)
        buf.ptr[j] = value + j * delta;
    }
  }
}

void AXUCode::DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb) {
  int* buffers_main[3] = {m_samples_main_left, m_samples_main_right, m_samples_main_surround};
  int* buffers_auxa[3] = {m_samples_auxA_left, m_samples_auxA_right, m_samples_auxA_surround};
  int* buffers_auxb[3] = {m_samples_auxB_left, m_samples_auxB_right, m_samples_auxB_surround};
  int** buffers[3] = {buffers_main, buffers_auxa, buffers_auxb};
  const u16 volumes[3] = {vol_main, vol_auxa, vol_auxb};

  // Dolphin re-reads the same 3 * 5 * 32 words from addr for each of the three
  // destinations (the pointer is fetched inside the loop); kept as is.
  int src[3 * 5 * 32];
  read_emu_s32(src, addr, 3 * 5 * 32);
  for (u32 i = 0; i < 3; ++i) {
    const int* ptr = src;
    const u16 volume = volumes[i];
    for (u32 j = 0; j < 3; ++j) {
      int* buffer = buffers[i][j];
      for (u32 k = 0; k < 5 * 32; ++k) {
        s64 sample = s64(s32(*ptr++));
        sample *= volume;
        buffer[k] += s32(sample >> 15);
      }
    }
  }
}

// Determines if this version of the UCode has a PBLowPassFilter in its AXPB layout.
bool HasLpf(u32 crc) {
  return crc != 0x4E8A8B21;
}

void AXUCode::ReadPB(u32 addr, AXPB& pb) const {
  if (HasLpf(m_crc)) {
    copy_from_emu_u16(reinterpret_cast<u16*>(&pb), addr, sizeof(pb));
  } else {
    char* dst = reinterpret_cast<char*>(&pb);
    constexpr size_t lpf_off = offsetof(AXPB, lpf);
    constexpr size_t lc_off = offsetof(AXPB, loop_counter);
    copy_from_emu_u16(reinterpret_cast<u16*>(dst), addr, lpf_off);
    std::memset(dst + lpf_off, 0, lc_off - lpf_off);
    copy_from_emu_u16(reinterpret_cast<u16*>(dst + lc_off), u32(addr + lpf_off), sizeof(pb) - lc_off);
  }
}

void AXUCode::WritePB(u32 addr, const AXPB& pb) const {
  if (HasLpf(m_crc)) {
    copy_to_emu_u16(addr, reinterpret_cast<const u16*>(&pb), sizeof(pb));
  } else {
    const char* src = reinterpret_cast<const char*>(&pb);
    constexpr size_t lpf_off = offsetof(AXPB, lpf);
    constexpr size_t lc_off = offsetof(AXPB, loop_counter);
    copy_to_emu_u16(addr, reinterpret_cast<const u16*>(src), lpf_off);
    copy_to_emu_u16(u32(addr + lpf_off), reinterpret_cast<const u16*>(src + lc_off), sizeof(pb) - lc_off);
  }
}

void AXUCode::ProcessPBList(u32 pb_addr) {
  constexpr u32 spms = 32;

  AXPB pb;
  int guard = 0;
  while (pb_addr) {
    if (++guard > 4096) {  // a cyclic PB list; Dolphin would spin forever
      std::fprintf(stderr, "ax_hle: PB list does not terminate\n");
      break;
    }
    AXBuffers buffers = {m_samples_main_left,  m_samples_main_right, m_samples_main_surround,
                         m_samples_auxA_left,  m_samples_auxA_right, m_samples_auxA_surround,
                         m_samples_auxB_left,  m_samples_auxB_right, m_samples_auxB_surround};

    ReadPB(pb_addr, pb);

    const PBUpdateData updates = LoadPBUpdates(pb);

    for (int curr_ms = 0; curr_ms < 5; ++curr_ms) {
      ApplyUpdatesForMs(curr_ms, pb, pb.updates.num_updates, updates);

      ProcessVoice(&m_accelerator, pb, buffers, spms, ConvertMixerControl(pb.mixer_control),
                   m_coeffs_valid ? m_coeffs.data() : nullptr);

      int** ptrs = buffers.ptrs();
      for (int p = 0; p < 9; ++p)
        ptrs[p] += spms;
    }

    WritePB(pb_addr, pb);
    pb_addr = HILO_TO_32(pb.next_pb);
  }
}

void AXUCode::MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr) {
  int* buffers[3] = {nullptr};

  switch (aux_id) {
  case 0:
    buffers[0] = m_samples_auxA_left;
    buffers[1] = m_samples_auxA_right;
    buffers[2] = m_samples_auxA_surround;
    break;
  case 1:
    buffers[0] = m_samples_auxB_left;
    buffers[1] = m_samples_auxB_right;
    buffers[2] = m_samples_auxB_surround;
    break;
  }

  if (write_addr) {
    for (auto& buffer : buffers) {
      copy_to_emu_s32(write_addr, buffer, 5 * 32 * sizeof(int));
      write_addr += 5 * 32 * sizeof(int);
    }
  }

  int src[3 * 5 * 32];
  read_emu_s32(src, read_addr, 3 * 5 * 32);
  const int* ptr = src;
  for (auto& sample : m_samples_main_left)
    sample += *ptr++;
  for (auto& sample : m_samples_main_right)
    sample += *ptr++;
  for (auto& sample : m_samples_main_surround)
    sample += *ptr++;
}

void AXUCode::UploadLRS(u32 dst_addr) {
  for (const int* samples : {m_samples_main_left, m_samples_main_right, m_samples_main_surround}) {
    copy_to_emu_s32(dst_addr, samples, 5 * 32 * sizeof(int));
    dst_addr += 5 * 32 * sizeof(int);
  }
}

void AXUCode::SetMainLR(u32 src_addr) {
  int src[5 * 32];
  read_emu_s32(src, src_addr, 5 * 32);
  for (u32 i = 0; i < 5 * 32; ++i) {
    const int samp = src[i];
    m_samples_main_left[i] = samp;
    m_samples_main_right[i] = samp;
    m_samples_main_surround[i] = 0;
  }
}

void AXUCode::RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis) {
  bool triggered = false;
  for (u32 i = 0; i < 32 * millis; ++i) {
    if (std::abs(m_samples_main_left[i]) > int(threshold) || std::abs(m_samples_main_right[i]) > int(threshold)) {
      triggered = true;
      break;
    }
  }

  const u32 frame_byte_size = 32 * millis * sizeof(s16);
  u32 table_offset = 0;
  if (triggered) {
    table_offset = m_compressor_pos * frame_byte_size;
    m_compressor_pos = release_frames;
  } else if (m_compressor_pos) {
    --m_compressor_pos;
    constexpr u32 ATTACK_ENTRY_COUNT = 11;
    table_offset = (ATTACK_ENTRY_COUNT + m_compressor_pos) * frame_byte_size;
  } else {
    return;
  }

  u16 ramp[32 * 5];
  copy_from_emu_u16(ramp, table_addr + table_offset, 32 * millis * sizeof(u16));
  for (u32 i = 0; i < 32 * millis; ++i) {
    const u16 coef = ramp[i];
    m_samples_main_left[i] = int((s64(m_samples_main_left[i]) * coef) >> 15);
    m_samples_main_right[i] = int((s64(m_samples_main_right[i]) * coef) >> 15);
  }
}

void AXUCode::OutputSamples(u32 lr_addr, u32 surround_addr) {
  copy_to_emu_s32(surround_addr, m_samples_main_surround, 5 * 32 * sizeof(int));

  // 32 samples per ms, 5 ms, 2 channels, interlaced RLRL... big-endian
  u16 buffer[5 * 32 * 2];
  for (u32 i = 0; i < 5 * 32; ++i) {
    const s16 left = ClampS16(m_samples_main_left[i]);
    const s16 right = ClampS16(m_samples_main_right[i]);
    buffer[2 * i + 0] = u16(right);
    buffer[2 * i + 1] = u16(left);
  }
  copy_to_emu_u16(lr_addr, buffer, sizeof(buffer));
}

void AXUCode::MixAUXBLR(u32 ul_addr, u32 dl_addr) {
  copy_to_emu_s32(ul_addr, m_samples_auxB_left, sizeof(m_samples_auxB_left));
  copy_to_emu_s32(ul_addr + sizeof(m_samples_auxB_left), m_samples_auxB_right, sizeof(m_samples_auxB_right));

  int src[2 * 5 * 32];
  read_emu_s32(src, dl_addr, 2 * 5 * 32);
  const int* ptr = src;
  for (u32 i = 0; i < 5 * 32; ++i) {
    const int samp = *ptr++;
    m_samples_auxB_left[i] = samp;
    m_samples_main_left[i] += samp;
  }
  for (u32 i = 0; i < 5 * 32; ++i) {
    const int samp = *ptr++;
    m_samples_auxB_right[i] = samp;
    m_samples_main_right[i] += samp;
  }
}

void AXUCode::SetOppositeLR(u32 src_addr) {
  int src[5 * 32];
  read_emu_s32(src, src_addr, 5 * 32);
  for (u32 i = 0; i < 5 * 32; ++i) {
    const int inp = src[i];
    m_samples_main_left[i] = -inp;
    m_samples_main_right[i] = inp;
    m_samples_main_surround[i] = 0;
  }
}

void AXUCode::SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                            u32 auxb_r_dl) {
  copy_to_emu_s32(auxa_lrs_up, m_samples_auxA_left, 32 * 5 * sizeof(int));
  copy_to_emu_s32(auxa_lrs_up + 32 * 5 * sizeof(int), m_samples_auxA_right, 32 * 5 * sizeof(int));
  copy_to_emu_s32(auxa_lrs_up + 2 * 32 * 5 * sizeof(int), m_samples_auxA_surround, 32 * 5 * sizeof(int));

  copy_to_emu_s32(auxb_s_up, m_samples_auxB_surround, sizeof(m_samples_auxB_surround));

  int* const dl_buffers[4] = {m_samples_main_left, m_samples_main_right, m_samples_auxB_left,
                              m_samples_auxB_right};
  const u32 dl_addrs[4] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};

  for (size_t i = 0; i < 4; ++i) {
    int src[32 * 5];
    read_emu_s32(src, dl_addrs[i], 32 * 5);
    for (size_t j = 0; j < 32 * 5; ++j)
      dl_buffers[i][j] += src[j];
  }
}

void AXUCode::HandleMail(u32 mail) {
  if (mode == Mode::Card) {
    // CARDUCode::HandleMail: 0xFF000000 unlocks the card; any mail ends the task.
    push_mail(DSP_DONE, true);  // CARDUCode::Update raises the interrupt
    mode = Mode::Rom;           // SetUCode(UCODE_ROM)
    return;
  }
  if (mode != Mode::AX)
    return;

  if (m_upload_setup_in_progress) {
    PrepareBootUCode(mail);
    return;
  }

  switch (m_mail_state) {
  case MailState::WaitingForCmdListSize:
    if ((mail & MAIL_CMDLIST_MASK) == MAIL_CMDLIST) {
      m_cmdlist_size = static_cast<u16>(mail & ~MAIL_CMDLIST_MASK);
      m_mail_state = MailState::WaitingForCmdListAddress;
    } else {
      std::fprintf(stderr, "ax_hle: unknown mail, expected command list: %08x\n", mail);
    }
    break;

  case MailState::WaitingForCmdListAddress:
    CopyCmdList(mail, u16(m_cmdlist_size));
    HandleCommandList();
    m_cmdlist_size = 0;
    SignalWorkEnd();
    m_mail_state = MailState::WaitingForNextTask;
    break;

  case MailState::WaitingForNextTask:
    if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP) {
      // The actual uCode does not check for the CDD1 prefix.
      mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
    }

    switch (mail) {
    case MAIL_RESUME:
      push_mail(DSP_RESUME, true);
      m_mail_state = MailState::WaitingForCmdListSize;
      break;

    case MAIL_NEW_UCODE:
      m_upload_setup_in_progress = true;
      m_mail_state = MailState::WaitingForCmdListSize;
      break;

    case MAIL_RESET:
      mode = Mode::Rom;  // m_dsphle->SetUCode(UCODE_ROM)
      saved_ax = false;
      break;

    case MAIL_CONTINUE:
      m_mail_state = MailState::WaitingForCmdListSize;
      break;

    default:
      std::fprintf(stderr, "ax_hle: unknown task mail: %08x\n", mail);
      break;
    }
    break;
  }
}

// UCodeInterface::PrepareBootUCode: the 10 mails describing the next task.
void AXUCode::PrepareBootUCode(u32 mail) {
  switch (m_next_ucode_steps) {
  case 3: m_next_iram_mram_addr = mail; break;
  case 4: m_next_iram_size = u16(mail & 0xffff); break;
  default: break;
  }
  m_next_ucode_steps++;

  if (m_next_ucode_steps == 10) {
    m_next_ucode_steps = 0;
    m_needs_resume_mail = true;
    m_upload_setup_in_progress = false;

    const u8* pointer = mem_ptr(m_next_iram_mram_addr, m_next_iram_size);
    const u32 ector_crc = pointer ? ax_hash_ector(pointer, m_next_iram_size) : 0;
    SwapUCode(ector_crc);
  }
}

// DSPHLE::SwapUCode, for the programs this host knows: AX (kept and resumed)
// and CARD. Anything else is booted as a fresh AX, like Dolphin's fallback.
void AXUCode::SwapUCode(u32 crc) {
  if (std::getenv("STANDALONE_DSP_LOG"))
    std::printf("  dsp ucode switch to 0x%08X\n", crc);
  if (saved_ax && crc == m_crc) {
    saved_ax = false;
    mode = Mode::AX;  // the resume mail goes out on the next Update
    return;
  }
  saved_ax = true;
  if (crc == CARD_UCODE_CRC) {
    mode = Mode::Card;
    push_mail(DSP_INIT, true);  // CARDUCode::Initialize; Update raises the interrupt
    return;
  }
  std::fprintf(stderr, "ax_hle: switching to ucode 0x%08X, booting it as AX\n", crc);
  saved_ax = false;
  m_crc = crc;
  ResetState();
  mode = Mode::AX;
  push_mail(DSP_INIT, true);
}

void AXUCode::CopyCmdList(u32 addr, u16 size) {
  if (size >= kCmdListWords) {
    std::fprintf(stderr, "ax_hle: command list at %08x is too large: size=%u\n", addr, size);
    return;
  }
  copy_from_emu_u16(m_cmdlist, addr, size * sizeof(u16));
}

void AXUCode::Update() {
  // Used for UCode switching (NeedsResumeMail).
  if (mode == Mode::AX && m_needs_resume_mail) {
    m_needs_resume_mail = false;
    push_mail(DSP_RESUME, true);
  }
}

}  // namespace

void ax_init(const AxHostInterface& host, uint32_t crc) {
  g_host = host;
  if (std::getenv("STANDALONE_DSP_LOG"))
    std::printf("  dsp AX task boots, ucode 0x%08X\n", crc);
  g_ax.Initialize(crc);
}

void ax_handle_mail(uint32_t mail) {
  g_ax.HandleMail(mail);
}

void ax_update() {
  g_ax.Update();
}

bool ax_active() {
  return g_ax.mode != AXUCode::Mode::Rom;
}

uint32_t ax_hash_ector(const uint8_t* data, uint32_t length) {
  uint32_t crc = 0;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}

uint64_t ax_command_lists() {
  return g_ax.command_lists;
}
