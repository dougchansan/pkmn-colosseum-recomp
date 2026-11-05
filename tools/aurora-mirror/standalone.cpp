// colosseum-standalone: the recompiled game on GXRuntime, without Dolphin.
//
//   colosseum-standalone [--verbose] [<game.iso> [<gGC6E01_recomp.dll>]]
//
// Without arguments the disc image and module come from the settings file
// (host_config.h), or are found next to the executable (see STANDALONE.md).
//
// The module (DolRecomp output, CPU ABI v4) runs here exactly as it runs inside
// ModernGekko, but everything the Dolphin fork supplied around it is local:
// guest memory, the boot the IPL and apploader would do, the timebase and
// decrementer, VI retrace, the processor interface's interrupts, the hardware
// registers the game touches, disc reads from the image, and the GX command
// stream, which goes straight to the same Aurora renderer as native mode
// (native.cpp, compiled into this executable).
//
// Status: boots through New Game and its opening into the first field, which
// plays. The DSP runs the audio task as AX HLE (ax_hle.cpp) and the audio DMA
// feeds SDL (audio_out.cpp; STANDALONE_AUDIO, STANDALONE_AUDIO_DUMP). Port 1 is the host's gamepad or keyboard
// (pad_input.cpp; STANDALONE_INPUT_SCRIPT for test runs); slot A holds a
// memory card backed by a raw image (memcard.cpp; STANDALONE_CARD, default
// MemoryCardA.USA.raw in the settings folder). Esc opens the pause menu
// (host_ui.cpp).

#include "gxruntime/utf8_file.h"
#include "audio_out.h"
#include "ax_hle.h"
#include "host_config.h"
#include "host_ui.h"
#include "memcard.h"
#include "native_text.h"
#include "pad_input.h"
#include "presentation.h"
#include "idle_wait.h"
#include "frame_rate.h"
#include "linux_guest_ram.h"

#include <SDL3/SDL.h>

extern "C" {
#include "core/cpu.h"
#include "gxruntime/di.h"
#include "gxruntime/dvd.h"
#include "gxruntime/exi.h"
#include "gxruntime/interrupts.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

// native.cpp, compiled into this executable.
extern "C" {
bool aurora_native_init(const uint8_t* ram, uint32_t ram_size);
void aurora_native_feed(const uint8_t* data, uint32_t size);
void aurora_native_frame_end(float aspect);
void aurora_native_set_frame_rate(unsigned rate, bool allowed, unsigned fields);
void aurora_native_configure_texture_pack(const char* path);
void aurora_native_sync();
bool aurora_native_should_quit();
void aurora_native_shutdown();
void aurora_native_configure(unsigned width, unsigned height, bool vsync);
void aurora_native_set_verbose(bool verbose);
void aurora_native_repaint();
void aurora_native_request_quit();
void aurora_native_set_exact_aspect(bool exact);
void aurora_native_set_text_rasterizer(AuroraTextRasterizer rasterizer);
void aurora_native_use_main_thread_renderer();
void aurora_native_wake_main_thread();
void aurora_native_run_main_thread(void (*guest)(void*), void* data, void (*poll_main_thread)());
}

// The interpreter for guest code the module does not contain (gekko_interp.cpp).
bool gekko_interpret(CPUState* cpu);

namespace {

// Status lines (--verbose, STANDALONE_VERBOSE=1 or verbose = 1 in the settings).
bool g_verbose = false;
#define VLOG(...)                 \
  do {                            \
    if (g_verbose)                \
      std::printf(__VA_ARGS__);   \
  } while (0)

// --- The module -------------------------------------------------------------

// ModernGekkoModuleDesc (moderngekko/module_abi.h, ABI v3).
struct Range {
  uint32_t start, end;
};
struct RelSection {
  uint32_t module_id, section_index, linked_start, size;
};
struct RelModule {
  uint32_t module_id, version, section_count, section_info_offset, file_size;
  const RelSection* sections;
  uint32_t num_sections;
};
struct ModuleDesc {
  uint32_t abi_version;
  uint32_t cpu_abi_version;
  uint32_t cpu_state_size;
  char game_id[8];
  uint32_t entry_point;
  int (*dispatch)(CPUState* state, uint32_t address);
  void (*on_state_loaded)(CPUState* state);
  const Range* code_ranges;
  uint32_t num_code_ranges;
  const Range* smc_ranges;
  uint32_t num_smc_ranges;
  const Range* chunk_ranges;
  uint32_t num_chunk_ranges;
  const uint64_t* chunk_hashes;
  const RelModule* rel_modules;
  uint32_t num_rel_modules;
};
using GetModuleFn = const ModuleDesc* (*)();

bool g_module_alias_safe = false;
const ModuleDesc* load_module(const char* path) {
#ifdef _WIN32
  wchar_t* wide_path = dol_utf8_to_wide(path);
  HMODULE dll = wide_path ? LoadLibraryW(wide_path) : nullptr;
  std::free(wide_path);
  if (!dll) {
    std::fprintf(stderr, "standalone: cannot load %s (error %lu)\n", path, GetLastError());
    return nullptr;
  }
  auto get = reinterpret_cast<GetModuleFn>(GetProcAddress(dll, "staticrecomp_get_module"));
#else
  void* dll = dlopen(path, RTLD_NOW);
  if (!dll) {
    std::fprintf(stderr, "standalone: cannot load %s: %s\n", path, dlerror());
    return nullptr;
  }
  auto get = reinterpret_cast<GetModuleFn>(dlsym(dll, "staticrecomp_get_module"));
#endif
  const ModuleDesc* desc = get ? get() : nullptr;
  if (!desc || desc->abi_version != 3 || desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION ||
      desc->cpu_state_size != sizeof(CPUState)) {
    std::fprintf(stderr, "standalone: %s is not a compatible module (abi %u, cpu abi %u, state %u)\n",
                 path, desc ? desc->abi_version : 0, desc ? desc->cpu_abi_version : 0,
                 desc ? desc->cpu_state_size : 0);
    return nullptr;
  }
  g_module_alias_safe = false;
#if defined(__linux__) && defined(__x86_64__)
  auto memory_features = reinterpret_cast<uint32_t (*)()>(dlsym(dll, "staticrecomp_memory_features_v1"));
  g_module_alias_safe = memory_features && (memory_features() & 1u);
#endif
  return desc;
}

// --- Guest machine ------------------------------------------------------------

constexpr uint32_t kRamSize = 0x01800000;   // MEM1, 24 MB
constexpr uint32_t kAramSize = 0x01000000;  // ARAM, 16 MB
constexpr uint64_t kCpuHz = 486000000;      // Gekko
constexpr uint32_t kCyclesPerTick = 12;     // timebase runs at bus clock / 4
// NTSC fields: 525 lines at 15.734 kHz, 59.94 per second.
constexpr uint64_t kCyclesPerField = kCpuHz * 1001 / 60000;

#if defined(__linux__) && defined(__x86_64__)
LinuxGuestRam g_ram;
static_assert(LinuxGuestRam::kRamSize == kRamSize);
uint8_t* const g_l1 = g_ram.locked_cache();
#else
std::vector<uint8_t> g_ram(kRamSize);
uint8_t g_l1[0x4000];
#endif
std::vector<uint8_t> g_aram(kAramSize);
constexpr uint32_t kLockedCacheSize = 0x4000; // guest 0xE0000000
CPUState g_cpu;

uint64_t g_fields = 0;
bool g_interpolation_catchup = false;
uint64_t g_interpolation_catchup_fields = 0;
uint64_t g_cycles = 0;         // guest cycles since boot
uint64_t g_tick_remainder = 0; // cycles not yet a timebase tick
uint64_t g_next_field = kCyclesPerField;
uint32_t g_dec = 0xFFFFFFFF;   // decrementer, counted down with the timebase
bool g_dec_pending = false;
uint32_t g_spr[1024];

DolInterrupts g_pi;
DolDi g_di;
DolExi g_exi;

// The serial interface (controllers), defined further down.
bool si_contains(uint32_t a);
uint64_t si_read(uint32_t a, uint8_t size);
void si_write(uint32_t a, uint8_t size, uint64_t value);
void si_poll();

uint64_t g_stat_unknown_reads = 0, g_stat_unknown_writes = 0, g_stat_external = 0, g_stat_decrementer = 0;

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

uint8_t* guest(uint32_t address) {
  const uint32_t physical = address & 0x1FFFFFFF;
  return physical < kRamSize ? g_ram.data() + physical : nullptr;
}

void warn_once(const char* what, uint32_t address) {
  static std::vector<uint32_t> seen;
  if (std::find(seen.begin(), seen.end(), address) != seen.end() || seen.size() > 256)
    return;
  seen.push_back(address);
  std::fprintf(stderr, "standalone: %s 0x%08X (pc 0x%08X)\n", what, address, g_cpu.pc);
}

// --- GX: the command stream ------------------------------------------------------

// Vertex sizes from the CP's descriptor (VCD) and attribute formats (VAT): what
// it takes to step over a draw in the stream.
uint32_t g_vcd_lo = 0, g_vcd_hi = 0;
uint32_t g_vat[3][8];
uint32_t g_pe_token = 0;
bool g_pe_token_pending = false;

uint32_t component_bytes(uint32_t format) {
  static const uint32_t sizes[8] = {1, 1, 2, 2, 4, 0, 0, 0};
  return sizes[format & 7];
}

uint32_t color_bytes(uint32_t format) {
  static const uint32_t sizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
  return sizes[format & 7];
}

uint32_t vertex_size(uint32_t fmt) {
  const uint32_t a = g_vat[0][fmt], b = g_vat[1][fmt], c = g_vat[2][fmt];
  uint32_t size = 0;
  for (int i = 0; i < 9; ++i)  // position/normal matrix index, 8 texture matrix indices
    size += (g_vcd_lo >> i) & 1;
  const auto attribute = [&](uint32_t mode, uint32_t direct) {
    size += mode == 1 ? direct : mode == 2 ? 1 : mode == 3 ? 2 : 0;
  };
  attribute((g_vcd_lo >> 9) & 3, component_bytes((a >> 1) & 7) * (a & 1 ? 3 : 2));
  const uint32_t normal_mode = (g_vcd_lo >> 11) & 3;
  if (normal_mode) {
    const bool nbt = (a >> 9) & 1;
    if (normal_mode == 1)
      size += component_bytes((a >> 10) & 7) * (nbt ? 9 : 3);
    else
      size += (normal_mode == 2 ? 1 : 2) * (nbt && ((a >> 31) & 1) ? 3 : 1);
  }
  attribute((g_vcd_lo >> 13) & 3, color_bytes((a >> 14) & 7));
  attribute((g_vcd_lo >> 15) & 3, color_bytes((a >> 18) & 7));
  const uint32_t tex_format[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7,
                                  (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
  const uint32_t tex_count[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1,
                                 (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
  for (int t = 0; t < 8; ++t)
    attribute((g_vcd_hi >> (2 * t)) & 3, component_bytes(tex_format[t]) * (tex_count[t] ? 2 : 1));
  return size;
}

std::vector<uint8_t> g_fifo;  // gather-pipe bytes not yet a whole command
// The stream from the last XFB copy on (g_frame_building) and the frame before
// it, from its opening copy (which clears the EFB) through its draws
// (g_last_frame): what is fed again to show the frame while paused.
std::vector<uint8_t> g_frame_building, g_last_frame;
uint64_t g_stat_frames = 0, g_stat_draw_done = 0, g_stat_bad_commands = 0;

void on_bp(uint32_t value) {
  switch (value >> 24) {
  case 0x45:  // draw done
    aurora_native_sync();
    dol_interrupts_commit_pe_finish(&g_pi);
    ++g_stat_draw_done;
    break;
  case 0x47:  // token, with interrupt
    g_pe_token = value & 0xFFFF;
    g_pe_token_pending = true;
    dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_PE_TOKEN, true);
    break;
  case 0x48:
    g_pe_token = value & 0xFFFF;
    break;
  case 0x52:  // EFB copy; to the XFB ends a frame
    if ((value >> 14) & 1) {
      // The guest has drawn this frame under the settled projection; keep it
      // in place for the next (ModernGekko's before_frame hook).
      presentation_enforce(g_ram.data(), kRamSize);
      static uint64_t previous_frame_field = 0;
      const unsigned frame_fields = unsigned(std::clamp<uint64_t>(g_fields - previous_frame_field, 1, 2));
      previous_frame_field = g_fields;
      aurora_native_set_frame_rate(host_ui_frame_rate(),
                                   presentation_interpolation_allowed() && !g_interpolation_catchup, frame_fields);
      aurora_native_frame_end(presentation_present_aspect());
      ++g_stat_frames;
    }
    break;
  }
}

// Splits the stream into whole commands, acts on the few the machine itself must
// answer, and hands the rest to the renderer unchanged.
void drain_fifo() {
  const uint8_t* data = g_fifo.data();
  const size_t size = g_fifo.size();
  size_t pos = 0;
  while (pos < size) {
    const uint8_t cmd = data[pos];
    size_t length = 0;
    if (cmd == 0x00 || cmd == 0x48) {  // nop, invalidate vertex cache
      length = 1;
    } else if (cmd == 0x08) {  // CP register
      if (pos + 6 > size)
        break;
      const uint8_t reg = data[pos + 1];
      const uint32_t value = rd32(data + pos + 2);
      if (reg == 0x50)
        g_vcd_lo = value;
      else if (reg == 0x60)
        g_vcd_hi = value;
      else if (reg >= 0x70 && reg < 0xA0)
        g_vat[(reg - 0x70) >> 4][reg & 7] = value;
      length = 6;
    } else if (cmd == 0x10) {  // XF registers
      if (pos + 5 > size)
        break;
      length = 5 + 4 * ((rd32(data + pos + 1) >> 16) + 1);
    } else if ((cmd & 0xE7) == 0x20) {  // indexed XF load A-D
      length = 5;
    } else if (cmd == 0x40) {  // call display list
      length = 9;
    } else if (cmd == 0x61) {  // BP register
      if (pos + 5 > size)
        break;
      length = 5;
    } else if (cmd >= 0x80 && cmd < 0xC0) {  // draw
      if (pos + 3 > size)
        break;
      length = 3 + size_t(rd16(data + pos + 1)) * vertex_size(cmd & 7);
    } else {
      // Not a command: the stream is out of step. Drop what is buffered.
      ++g_stat_bad_commands;
      warn_once("unknown GX command at stream byte", cmd);
      g_fifo.clear();
      return;
    }
    if (pos + length > size)
      break;
    if (cmd == 0x61) {
      const uint32_t bp = rd32(data + pos + 1);
      if ((bp >> 24) == 0x52 && ((bp >> 14) & 1)) {
        g_last_frame.swap(g_frame_building);
        g_frame_building.clear();
      }
      on_bp(bp);
    }
    g_frame_building.insert(g_frame_building.end(), data + pos, data + pos + length);
    aurora_native_feed(data + pos, uint32_t(length));
    pos += length;
  }
  g_fifo.erase(g_fifo.begin(), g_fifo.begin() + ptrdiff_t(pos));
}

void gather_pipe_write(uint64_t value, uint8_t size) {
  for (int i = size - 1; i >= 0; --i)
    g_fifo.push_back(uint8_t(value >> (8 * i)));
  if (g_fifo.size() >= 32)
    drain_fifo();
}

// --- Hardware registers -------------------------------------------------------

uint8_t g_cp_regs[0x80];   // 0xCC000000
uint8_t g_pe_regs[0x100];  // 0xCC001000
uint8_t g_mi_regs[0x80];   // 0xCC004000
uint8_t g_pi_fifo[0x40];   // 0xCC003000 beyond cause/mask (FIFO pointers, reset)
uint8_t g_ai_regs[0x20];   // 0xCC006C00
uint32_t ai_dma_rate();
uint64_t ai_dma_cycles();
void ai_dma_tick();

// DSP (0xCC005000): mailboxes, control/status, ARAM DMA, audio DMA. The OS's
// boot handshake (__OSInitAudioSystem) is answered here: reset completes at
// once, ARAM DMA copies and raises its interrupt bit, and the init code's mail
// arrives once the DSP is let run. The audio task booted by __DSP_boot_task
// then runs as AX HLE (ax_hle.cpp), which gets every later CPU -> DSP mail.
struct DspMail {
  uint32_t mail;
  bool interrupt_on_read;  // CMailHandler: raise the DSP interrupt once this is read
};

struct Dsp {
  uint16_t mail_to_dsp[2];
  std::deque<DspMail> mail_to_cpu;  // head visible at 0xCC005004/6
  uint32_t last_mail_to_cpu = 0;
  uint16_t csr = 0;
  uint32_t ar_mm = 0, ar_aram = 0, ar_count = 0;
  uint16_t aram_size = 0, aram_mode = 0, aram_refresh = 0;
  uint32_t ai_dma_start = 0;
  uint16_t ai_dma_control = 0;
  uint32_t mails_received = 0, mails_sent = 0;
  // __DSP_boot_task's sequence: the mail after 0x80F3A001 is the task's IRAM
  // image address, after 0x80F3A002 its length; 0x80F3D001 precedes the init
  // vector, after which the task runs.
  uint32_t boot_prev_mail = 0;
  uint32_t task_iram_addr = 0, task_iram_length = 0;
  uint64_t ai_dma_due = 0;  // guest cycle the next audio DMA block is read (0: stopped)
  // Dolphin's DSPManager::UpdateAudioDMA: the playing buffer is latched from
  // 0x5030/32 and the block count when it starts and each time it wraps.
  uint32_t ai_dma_current = 0;
  uint16_t ai_dma_remaining = 0;
} g_dsp;

constexpr uint16_t kDspReset = 0x0001, kDspAidInt = 0x0008, kDspAramInt = 0x0020,
                   kDspDspInt = 0x0080, kDspHalt = 0x0004;

void dsp_sync_interrupt() {
  const uint16_t pending = g_dsp.csr & (kDspAidInt | kDspAramInt | kDspDspInt);
  const uint16_t enabled = (g_dsp.csr >> 1) & (kDspAidInt | kDspAramInt | kDspDspInt);
  dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_DSP, (pending & enabled) != 0);
}

void dsp_raise_interrupt() {
  g_dsp.csr |= kDspDspInt;
  dsp_sync_interrupt();
}

// DSP -> CPU mail, queued like Dolphin's CMailHandler::PushMail.
void dsp_push_mail(uint32_t mail, bool interrupt) {
  ++g_dsp.mails_sent;
  if (interrupt) {
    if (g_dsp.mail_to_cpu.empty())
      dsp_raise_interrupt();
    else
      g_dsp.mail_to_cpu.front().interrupt_on_read = true;
  }
  g_dsp.mail_to_cpu.push_back({mail, false});
}

void dsp_start_task() {
  uint32_t crc = 0;
  if (const uint8_t* image = guest(0x80000000 | g_dsp.task_iram_addr);
      image && (g_dsp.task_iram_addr & 0x1FFFFFFF) + g_dsp.task_iram_length <= kRamSize)
    crc = ax_hash_ector(image, g_dsp.task_iram_length);
  AxHostInterface host{};
  host.ram = g_ram.data();
  host.ram_size = kRamSize;
  host.aram = g_aram.data();
  host.aram_size = kAramSize;
  host.push_mail = dsp_push_mail;
  ax_init(host, crc);
}

void aram_dma() {
  const uint32_t length = g_dsp.ar_count & 0x7FFFFFFF;
  const bool to_mm = (g_dsp.ar_count >> 31) & 1;  // ARAM -> main memory
  uint8_t* mm = guest(0x80000000 | g_dsp.ar_mm);
  const uint32_t aram = g_dsp.ar_aram & (kAramSize - 1);
  if (mm && length && aram + length <= kAramSize &&
      (g_dsp.ar_mm & 0x1FFFFFFF) + length <= kRamSize) {
    if (to_mm)
      std::memcpy(mm, g_aram.data() + aram, length);
    else
      std::memcpy(g_aram.data() + aram, mm, length);
  }
  g_dsp.ar_count = 0;
  g_dsp.csr |= kDspAramInt;
  dsp_sync_interrupt();
}

uint64_t dsp_read(uint32_t off, uint8_t size) {
  uint16_t value = 0;
  switch (off & ~1u) {
  case 0x00: value = g_dsp.mail_to_dsp[0] & 0x7FFF; break;  // taken at once
  case 0x02: value = g_dsp.mail_to_dsp[1]; break;
  case 0x04:  // CMailHandler::ReadDSPMailboxHigh
    if (!g_dsp.mail_to_cpu.empty())
      g_dsp.last_mail_to_cpu = g_dsp.mail_to_cpu.front().mail;
    value = uint16_t(g_dsp.last_mail_to_cpu >> 16);
    break;
  case 0x06:  // ReadDSPMailboxLow: pops the head
    if (!g_dsp.mail_to_cpu.empty()) {
      const DspMail head = g_dsp.mail_to_cpu.front();
      g_dsp.mail_to_cpu.pop_front();
      g_dsp.last_mail_to_cpu = head.mail;
      if (head.interrupt_on_read)
        dsp_raise_interrupt();
    }
    g_dsp.last_mail_to_cpu &= ~0x80000000u;  // the high half reads back invalid
    value = uint16_t(g_dsp.last_mail_to_cpu);
    break;
  case 0x0A: value = g_dsp.csr; break;
  case 0x12: value = g_dsp.aram_size; break;
  case 0x16: value = g_dsp.aram_mode | 1; break;  // ARAM ready
  case 0x1A: value = g_dsp.aram_refresh; break;
  case 0x20: value = uint16_t(g_dsp.ar_mm >> 16); break;
  case 0x22: value = uint16_t(g_dsp.ar_mm); break;
  case 0x24: value = uint16_t(g_dsp.ar_aram >> 16); break;
  case 0x26: value = uint16_t(g_dsp.ar_aram); break;
  case 0x28: value = uint16_t(g_dsp.ar_count >> 16); break;
  case 0x2A: value = uint16_t(g_dsp.ar_count); break;
  case 0x30: value = uint16_t(g_dsp.ai_dma_start >> 16); break;
  case 0x32: value = uint16_t(g_dsp.ai_dma_start); break;
  case 0x36: value = g_dsp.ai_dma_control; break;
  case 0x3A:  // audio DMA blocks left (zero-based, as Dolphin)
    value = g_dsp.ai_dma_remaining ? g_dsp.ai_dma_remaining - 1 : 0;
    break;
  default: break;
  }
  if (size == 4)
    return (uint64_t(value) << 16) | dsp_read(off + 2, 2);
  return value;
}

void dsp_write(uint32_t off, uint8_t size, uint64_t value) {
  if (size == 4) {
    dsp_write(off, 2, value >> 16);
    dsp_write(off + 2, 2, value & 0xFFFF);
    return;
  }
  const uint16_t v = uint16_t(value);
  switch (off & ~1u) {
  case 0x00: g_dsp.mail_to_dsp[0] = v; break;
  case 0x02: {
    g_dsp.mail_to_dsp[1] = v;
    const uint32_t mail = (uint32_t(g_dsp.mail_to_dsp[0]) << 16) | v;
    ++g_dsp.mails_received;
    if (std::getenv("STANDALONE_DSP_LOG") && g_dsp.mails_received <= 400)
      std::printf("  dsp mail <- 0x%08X\n", mail);
    if (ax_active()) {
      ax_handle_mail(mail);
      break;
    }
    // The DSP ROM's task loader (Dolphin's ROMUCode).
    if (g_dsp.boot_prev_mail == 0x80F3A001)
      g_dsp.task_iram_addr = mail;
    else if (g_dsp.boot_prev_mail == 0x80F3A002)
      g_dsp.task_iram_length = mail & 0xFFFF;
    else if (g_dsp.boot_prev_mail == 0x80F3D001) {
      // The init vector: the task is running (AX pushes DSP_INIT).
      g_dsp.boot_prev_mail = 0;
      dsp_start_task();
      break;
    }
    g_dsp.boot_prev_mail = mail;
    break;
  }
  case 0x0A: {
    const uint16_t acks = v & (kDspAidInt | kDspAramInt | kDspDspInt);
    const bool was_halted = g_dsp.csr & kDspHalt;
    g_dsp.csr = uint16_t((g_dsp.csr & (kDspAidInt | kDspAramInt | kDspDspInt) & ~acks) |
                         (v & ~(kDspAidInt | kDspAramInt | kDspDspInt | kDspReset)));
    // Released from halt: the DSP's ROM (or the OS's init code) mails the CPU
    // that it is running. (0x0800 is the DSPInit flag, 0x0400 "init code
    // running", which reads back clear.)
    g_dsp.csr &= uint16_t(~0x0400);
    if (was_halted && !(g_dsp.csr & kDspHalt)) {
      g_dsp.mail_to_cpu.clear();
      dsp_push_mail(0x8071FEED, false);
    }
    dsp_sync_interrupt();
    break;
  }
  case 0x12: g_dsp.aram_size = v; break;
  case 0x16: g_dsp.aram_mode = v; break;
  case 0x1A: g_dsp.aram_refresh = v; break;
  case 0x20: g_dsp.ar_mm = (g_dsp.ar_mm & 0xFFFF) | (uint32_t(v) << 16); break;
  case 0x22: g_dsp.ar_mm = (g_dsp.ar_mm & 0xFFFF0000) | v; break;
  case 0x24: g_dsp.ar_aram = (g_dsp.ar_aram & 0xFFFF) | (uint32_t(v) << 16); break;
  case 0x26: g_dsp.ar_aram = (g_dsp.ar_aram & 0xFFFF0000) | v; break;
  case 0x28: g_dsp.ar_count = (g_dsp.ar_count & 0xFFFF) | (uint32_t(v) << 16); break;
  case 0x2A:
    g_dsp.ar_count = (g_dsp.ar_count & 0xFFFF0000) | v;
    aram_dma();
    break;
  case 0x30: g_dsp.ai_dma_start = (g_dsp.ai_dma_start & 0xFFFF) | (uint32_t(v) << 16); break;
  case 0x32: g_dsp.ai_dma_start = (g_dsp.ai_dma_start & 0xFFFF0000) | v; break;
  case 0x36: {
    const bool was_on = g_dsp.ai_dma_control & 0x8000;
    g_dsp.ai_dma_control = v;
    if ((v & 0x8000) && !was_on) {
      g_dsp.ai_dma_due = 1;  // start: scheduled by ai_dma_tick
      g_dsp.ai_dma_current = g_dsp.ai_dma_start;
      g_dsp.ai_dma_remaining = v & 0x7FFF;
      static bool logged = false;
      if (!logged)
        VLOG("standalone: audio DMA starts at %.3f s: %u blocks from 0x%08X, %u Hz\n",
                    double(g_cycles) / kCpuHz, v & 0x7FFF, g_dsp.ai_dma_start, ai_dma_rate());
      logged = true;
    } else if (!(v & 0x8000))
      g_dsp.ai_dma_due = 0;
    break;
  }
  default: break;
  }
}

uint32_t ai_dma_rate() { return (g_ai_regs[3] & 0x40) ? 32000 : 48000; }  // AICR AIDFR: 1 = 32 kHz

// One 32-byte block: 8 stereo frames.
uint64_t ai_dma_cycles() { return 8 * kCpuHz / ai_dma_rate(); }

// As Dolphin's UpdateAudioDMA, once per block: the block goes to the audio
// output, and when the buffer is done it restarts from the latched start
// address (the game has written the next one by then) and raises AIDINT.
void ai_dma_tick() {
  if (!g_dsp.ai_dma_due)
    return;
  if (g_dsp.ai_dma_due == 1) {
    g_dsp.ai_dma_due = g_cycles + ai_dma_cycles();
    return;
  }
  while (g_dsp.ai_dma_due && g_cycles >= g_dsp.ai_dma_due) {
    static const uint8_t silence[32] = {};
    const uint8_t* block = guest(0x80000000 | (g_dsp.ai_dma_current & 0x03FFFFE0));
    audio_out_push(block ? block : silence, 8, ai_dma_rate());
    if (g_dsp.ai_dma_remaining) {
      --g_dsp.ai_dma_remaining;
      g_dsp.ai_dma_current += 32;
    }
    if (!g_dsp.ai_dma_remaining) {
      g_dsp.ai_dma_current = g_dsp.ai_dma_start;
      g_dsp.ai_dma_remaining = g_dsp.ai_dma_control & 0x7FFF;
      g_dsp.csr |= kDspAidInt;
      dsp_sync_interrupt();
    }
    g_dsp.ai_dma_due = (g_dsp.ai_dma_control & 0x8000) ? g_dsp.ai_dma_due + ai_dma_cycles() : 0;
  }
}

uint64_t reg_read(const uint8_t* regs, uint32_t off, uint8_t size) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < size; ++i)
    value = (value << 8) | regs[off + i];
  return value;
}

void reg_write(uint8_t* regs, uint32_t off, uint8_t size, uint64_t value) {
  for (uint8_t i = 0; i < size; ++i)
    regs[off + size - 1 - i] = uint8_t(value >> (8 * i));
}

void sync_device_interrupts() {
  // As Dolphin's CEXIChannel::IsCausingInterrupt: a memory card's pending
  // interrupt latches EXIINT whatever is selected.
  if (memcard_interrupt())
    dol_exi_set_device_interrupt(&g_exi, 0, true);
  dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_DI, dol_di_interrupt_pending(&g_di));
  dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_EXI, dol_exi_interrupt_pending(&g_exi));
}

uint64_t mmio_read(uint32_t ea, uint8_t size) {
  const uint32_t a = 0xCC000000 | (ea & 0xFFFF);
  const uint32_t off = a & 0xFFF;
  switch (a & 0xFFFFF000) {
  case 0xCC000000:  // CP: always idle with nothing queued (the stream is drained as it arrives)
    if (off == 0x00)
      return 0x000C;  // read idle, command idle
    return off + size <= sizeof(g_cp_regs) ? reg_read(g_cp_regs, off, size) : 0;
  case 0xCC001000:  // PE
    if (off == 0x0A) {
      uint64_t status = dol_interrupts_mmio_read(&g_pi, a, size) | (reg_read(g_pe_regs, off, 2) & 0x3);
      if (g_pe_token_pending)
        status |= 0x4;
      return status;
    }
    if (off == 0x0E)
      return g_pe_token;
    return off + size <= sizeof(g_pe_regs) ? reg_read(g_pe_regs, off, size) : 0;
  case 0xCC002000:  // VI
    return dol_interrupts_mmio_read(&g_pi, a, size);
  case 0xCC003000:  // PI
    if (off < 0x08)
      return dol_interrupts_mmio_read(&g_pi, a, size);
    if (off == 0x2C)
      return 0x10000000;  // console revision
    return off + size <= sizeof(g_pi_fifo) ? reg_read(g_pi_fifo, off, size) : 0;
  case 0xCC004000:  // MI
    return off + size <= sizeof(g_mi_regs) ? reg_read(g_mi_regs, off, size) : 0;
  case 0xCC005000:
    return dsp_read(off, size);
  case 0xCC006000:
    if (dol_di_mmio_contains(a))
      return dol_di_mmio_read(&g_di, a, size);
    if (si_contains(a))
      return si_read(a, size);
    if (dol_exi_mmio_contains(a))
      return dol_exi_mmio_read(&g_exi, a, size);
    if (off >= 0xC00 && off < 0xC20) {
      if (off == 0xC08)  // AI sample counter: advances with the guest clock at 48 kHz
        return uint32_t(g_cycles / (kCpuHz / 48000));
      return reg_read(g_ai_regs, off - 0xC00, size);
    }
    break;
  }
  ++g_stat_unknown_reads;
  warn_once("unknown hardware read", a);
  return 0;
}

void mmio_write(uint32_t ea, uint64_t value, uint8_t size) {
  const uint32_t a = 0xCC000000 | (ea & 0xFFFF);
  const uint32_t off = a & 0xFFF;
  switch (a & 0xFFFFF000) {
  case 0xCC000000:
    if (off + size <= sizeof(g_cp_regs))
      reg_write(g_cp_regs, off, size, value);
    return;
  case 0xCC001000:
    if (off == 0x0A) {
      dol_interrupts_mmio_write(&g_pi, a, size, value);
      if (value & 0x4) {
        g_pe_token_pending = false;
        dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_PE_TOKEN, false);
      }
      reg_write(g_pe_regs, off, 2, value & 0x3);  // interrupt enables
      return;
    }
    if (off + size <= sizeof(g_pe_regs))
      reg_write(g_pe_regs, off, size, value);
    return;
  case 0xCC002000:
    dol_interrupts_mmio_write(&g_pi, a, size, value);
    return;
  case 0xCC003000:
    if (off < 0x08) {
      dol_interrupts_mmio_write(&g_pi, a, size, value);
      return;
    }
    if (off + size <= sizeof(g_pi_fifo))
      reg_write(g_pi_fifo, off, size, value);
    return;
  case 0xCC004000:
    if (off + size <= sizeof(g_mi_regs))
      reg_write(g_mi_regs, off, size, value);
    return;
  case 0xCC005000:
    dsp_write(off, size, value);
    return;
  case 0xCC006000:
    if (dol_di_mmio_contains(a)) {
      dol_di_mmio_write(&g_di, &g_cpu, a, size, value);
      sync_device_interrupts();
      return;
    }
    if (si_contains(a)) {
      si_write(a, size, value);
      return;
    }
    if (dol_exi_mmio_contains(a)) {
      // Slot A's card (channel 0, device 0) sees its chip select change.
      const uint32_t cs_before = g_exi.channels[0].status & DOL_EXI_STATUS_CHIP_SELECT_MASK;
      dol_exi_mmio_write(&g_exi, &g_cpu, a, size, value);
      const uint32_t cs_after = g_exi.channels[0].status & DOL_EXI_STATUS_CHIP_SELECT_MASK;
      if ((cs_before ^ cs_after) & (1u << 7))
        memcard_set_cs((cs_after & (1u << 7)) != 0, g_cycles);
      sync_device_interrupts();
      return;
    }
    if (off >= 0xC00 && off < 0xC20) {
      reg_write(g_ai_regs, off - 0xC00, size, value);
      return;
    }
    break;
  case 0xCC008000:
    gather_pipe_write(value, size);
    return;
  }
  ++g_stat_unknown_writes;
  warn_once("unknown hardware write", a);
}

// --- Serial interface: controllers -------------------------------------------------------

// The SI as the SDK drives it: per-channel output/input registers, the poll
// register, COMCSR (transfer control and interrupts), status (per-channel
// read/write/error bits) and the 128-byte I/O buffer. Port 1 holds a standard
// controller fed from the host (pad_input.cpp); ports 2-4 answer nothing, as
// empty ports do.
using PadState = PadInput;

struct Si {
  uint32_t out[4] = {};
  uint32_t in_hi[4] = {}, in_lo[4] = {};
  uint32_t poll = 0, comcsr = 0, status = 0, exi_clock = 0;
  uint8_t buffer[128] = {};
  bool present[4] = {true, false, false, false};
  PadState pad[4];
} g_si;

constexpr uint32_t kSiTcInt = 0x80000000, kSiTcIntMsk = 0x40000000, kSiComErr = 0x20000000,
                   kSiRdstInt = 0x10000000, kSiRdstIntMsk = 0x08000000, kSiTStart = 0x1;

uint32_t si_channel_shift(int channel) { return 24 - 8 * channel; }

void si_sync() {
  bool rdst = false;
  for (int c = 0; c < 4; ++c)
    rdst |= (g_si.status >> si_channel_shift(c)) & 0x20;
  g_si.comcsr = rdst ? (g_si.comcsr | kSiRdstInt) : (g_si.comcsr & ~kSiRdstInt);
  const bool pending = ((g_si.comcsr & kSiTcInt) && (g_si.comcsr & kSiTcIntMsk)) ||
                       ((g_si.comcsr & kSiRdstInt) && (g_si.comcsr & kSiRdstIntMsk));
  dol_interrupts_set_source(&g_pi, DOL_PI_CAUSE_SI, pending);
}

// A standard controller's poll reply in mode 3 (what the SDK's PADRead asks for).
void si_poll_reply(int c, uint32_t* hi, uint32_t* lo) {
  const PadState& p = g_si.pad[c];
  *hi = (uint32_t(p.buttons | 0x0080) << 16) | (uint32_t(p.stick_x) << 8) | p.stick_y;  // 0x80: use origin
  *lo = (uint32_t(p.c_x) << 24) | (uint32_t(p.c_y) << 16) | (uint32_t(p.trigger_l) << 8) | p.trigger_r;
}

// One transfer: OUTLNGTH bytes from the buffer to the channel's device, its
// reply (INLNGTH bytes) back into the buffer.
void si_transfer() {
  const int c = (g_si.comcsr >> 1) & 3;
  uint32_t out_length = (g_si.comcsr >> 16) & 0x7F, in_length = (g_si.comcsr >> 8) & 0x7F;
  if (!out_length) out_length = 128;
  if (!in_length) in_length = 128;
  const uint32_t shift = si_channel_shift(c);
  g_si.comcsr &= ~(kSiTStart | kSiComErr);
  if (!g_si.present[c]) {
    g_si.status |= 0x08u << shift;  // no response
    g_si.comcsr |= kSiComErr;
  } else {
    uint8_t reply[128] = {};
    uint32_t reply_length = 0;
    switch (g_si.buffer[0]) {
    case 0x00:  // type
    case 0xFF:  // reset
      reply[0] = 0x09; reply[1] = 0x00; reply[2] = 0x00;  // standard controller
      reply_length = 3;
      break;
    case 0x41:  // origin
    case 0x42:  // recalibrate
      reply[0] = 0x00; reply[1] = 0x80;
      reply[2] = reply[3] = reply[4] = reply[5] = 0x80;
      reply[6] = reply[7] = 0x00;
      reply_length = 10;
      break;
    case 0x40: {  // poll, as a direct command
      uint32_t hi, lo;
      si_poll_reply(c, &hi, &lo);
      wr32(reply, hi);
      wr32(reply + 4, lo);
      reply_length = 8;
      break;
    }
    default:
      warn_once("unknown SI command", g_si.buffer[0]);
      break;
    }
    std::memcpy(g_si.buffer, reply, std::min(in_length, uint32_t(sizeof(reply))));
    (void)reply_length;
  }
  g_si.comcsr |= kSiTcInt;
  si_sync();
}

// Once a field: every polled channel delivers its reply (or the absence of one).
void si_poll() {
  for (int c = 0; c < 4; ++c) {
    if (!(g_si.poll & (0x80u >> c)))
      continue;
    const uint32_t shift = si_channel_shift(c);
    if (g_si.present[c]) {
      si_poll_reply(c, &g_si.in_hi[c], &g_si.in_lo[c]);
    } else {
      g_si.in_hi[c] = 0x80000000;  // error bit: no reply
      g_si.in_lo[c] = 0;
      g_si.status |= 0x08u << shift;
    }
    g_si.status |= 0x20u << shift;  // read status: new data
  }
  si_sync();
}

bool si_contains(uint32_t a) { return a >= 0xCC006400 && a < 0xCC006500; }

uint64_t si_read(uint32_t a, uint8_t size) {
  const uint32_t off = a - 0xCC006400;
  if (off >= 0x80) {
    uint64_t v = 0;
    for (uint8_t i = 0; i < size && off - 0x80 + i < 128; ++i)
      v = (v << 8) | g_si.buffer[off - 0x80 + i];
    return v;
  }
  if (off < 0x30) {
    const int c = int(off / 0xC);
    switch (off % 0xC) {
    case 0: return g_si.out[c];
    case 4:
      g_si.status &= ~(0x20u << si_channel_shift(c));  // reading the reply clears read status
      si_sync();
      return g_si.in_hi[c];
    case 8: return g_si.in_lo[c];
    }
  }
  switch (off) {
  case 0x30: return g_si.poll;
  case 0x34: return g_si.comcsr;
  case 0x38: return g_si.status;
  case 0x3C: return g_si.exi_clock;
  }
  return 0;
}

void si_write(uint32_t a, uint8_t size, uint64_t value) {
  const uint32_t off = a - 0xCC006400;
  const uint32_t v = uint32_t(value);
  if (off >= 0x80) {
    for (uint8_t i = 0; i < size && off - 0x80 + i < 128; ++i)
      g_si.buffer[off - 0x80 + i] = uint8_t(value >> (8 * (size - 1 - i)));
    return;
  }
  if (off < 0x30) {
    if (off % 0xC == 0)
      g_si.out[off / 0xC] = v;
    return;
  }
  switch (off) {
  case 0x30: g_si.poll = v; break;
  case 0x34: {
    const uint32_t keep = 0x007F7F06u | kSiTcIntMsk | kSiRdstIntMsk;  // lengths, channel, masks
    g_si.comcsr = (g_si.comcsr & ~keep) | (v & keep);
    if (v & kSiTcInt)
      g_si.comcsr &= ~kSiTcInt;
    if (v & kSiTStart)
      si_transfer();
    si_sync();
    break;
  }
  case 0x38:
    g_si.status &= ~(v & 0x0F0F0F0Fu);  // errors are write-1-to-clear
    break;
  case 0x3C: g_si.exi_clock = v; break;
  }
  si_sync();
}

// --- Devices with payloads ----------------------------------------------------------

DolDiCommandResult di_command(void*, DolDiCommand* c) {
  const uint32_t op = c->command[0] >> 24;
  static const bool log = std::getenv("STANDALONE_DI_LOG") != nullptr;
  if (log)
    std::printf("  di 0x%08X 0x%08X 0x%08X dma 0x%08X+0x%X (field %llu)\n", c->command[0], c->command[1],
                c->command[2], c->dma_address, c->dma_length, (unsigned long long)(g_cycles / kCyclesPerField));
  switch (op) {
  case 0xA8:  // read (disc ID, or data)
  case 0xA9: {
    const uint32_t offset = c->command[1] << 2;
    const uint32_t length = c->dma ? c->dma_length : c->command[2];
    dvd_read_to_guest(c->cpu, 0x80000000 | c->dma_address, offset, length);
    return DOL_DI_COMMAND_COMPLETE;
  }
  case 0x12:  // inquiry
  case 0xAB:  // seek
  case 0xE3:  // stop motor
  case 0xE4:  // audio buffer configuration
    return DOL_DI_COMMAND_COMPLETE;
  case 0xE0:  // request error: none
    if (c->immediate_data)
      *c->immediate_data = 0;
    return DOL_DI_COMMAND_COMPLETE;
  default:
    warn_once("unknown disc command", c->command[0]);
    return DOL_DI_COMMAND_COMPLETE;
  }
}

// EXI channel 0, device 1 is the IPL: SRAM and the real-time clock. Channel 0,
// device 0 is the memory card in slot A (memcard.cpp). Everything else (slot B,
// the broadband adapter) is absent.
//
// SRAM as the SDK reads it from 0x800004 (Dolphin's Sram minus the RTC word),
// starting from Dolphin's template (Core/HW/Sram.cpp): English, stereo, set up.
uint8_t g_sram[64] = {
    0x00, 0x2C, 0xFF, 0xD0,  // checksums (fixed at boot)
    0, 0, 0, 0, 0, 0, 0, 0,  // ead0, ead1
    0, 0, 0, 0,              // rtc bias
    0, 0, 0, 0x2C,           // VI offset, ntd, language, flags
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'A',  // flash IDs
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'B',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // wireless IDs
    0, 0, 0x6E, 0x6D,        // DI error, unused, flash ID checksums
    0, 0, 0, 0,
};
constexpr uint32_t kSramFlashId = 0x14, kSramFlashIdChecksum = 0x3A;
uint32_t g_ipl_command = 0;
bool g_ipl_have_command = false;

// Dolphin's SetCardFlashID: the flash ID that unlocks a card, from its header.
void sram_set_card_flash_id(const uint8_t* header, int slot) {
  uint64_t rand = 0;
  for (int i = 0; i < 8; ++i)
    rand = (rand << 8) | header[12 + i];
  uint8_t csum = 0;
  for (int i = 0; i < 12; ++i) {
    rand = ((rand * 0x41C64E6Dull) + 0x3039ull) >> 16;
    csum += g_sram[kSramFlashId + 12 * slot + i] = uint8_t(header[i] - uint8_t(rand));
    rand = ((rand * 0x41C64E6Dull) + 0x3039ull) >> 16;
    rand &= 0x7FFFull;
  }
  g_sram[kSramFlashIdChecksum + slot] = csum ^ 0xFF;
}

// Dolphin's FixSRAMChecksums: over rtc_bias..flags.
void sram_fix_checksums() {
  uint16_t sum = 0, inv = 0;
  for (uint32_t off = 0x0C; off < 0x14; off += 2) {
    const uint16_t v = uint16_t(g_sram[off] << 8 | g_sram[off + 1]);
    sum = uint16_t(sum + v);
    inv = uint16_t(inv + uint16_t(~v));
  }
  g_sram[0] = uint8_t(sum >> 8); g_sram[1] = uint8_t(sum);
  g_sram[2] = uint8_t(inv >> 8); g_sram[3] = uint8_t(inv);
}

// Slot A's card: immediate transfers byte by byte, DMA at the address the
// command set up, as Dolphin's channel drives CEXIMemoryCard.
void memcard_transfer(DolExiTransfer* t) {
  if (t->dma) {
    uint8_t* mem = guest(0x80000000 | t->dma_address);
    if (!mem)
      return;
    if (t->direction == 0)
      memcard_dma_read(mem, t->length);
    else if (t->direction == 1)
      memcard_dma_write(mem, t->length);
    return;
  }
  if (t->direction == 0)
    *t->immediate_data = memcard_imm_read(t->length);
  else if (t->direction == 1)
    memcard_imm_write(*t->immediate_data, t->length);
}

bool exi_transfer(void*, DolExiTransfer* t) {
  static const bool log = std::getenv("STANDALONE_EXI_LOG") != nullptr;
  if (log)
    std::printf("  exi ch%u cs%u dir%u len %u dma %d addr 0x%08X imm 0x%08X (field %llu)\n", t->channel,
                t->chip_select, t->direction, t->length, t->dma, t->dma_address,
                t->immediate_data ? *t->immediate_data : 0, (unsigned long long)(g_cycles / kCyclesPerField));
  if (t->channel == 0 && t->chip_select == 1) {  // chip select bit 0 = device 0
    memcard_transfer(t);
    if (log && !t->dma && t->direction == 0)
      std::printf("    card -> 0x%08X\n", *t->immediate_data);
    return true;
  }
  if (t->channel != 0 || t->chip_select != 2) {  // chip select bit 1 = device 1
    if (t->immediate_data)
      *t->immediate_data = 0xFFFFFFFF;
    return true;
  }
  if (!t->dma && t->direction == 1 && !g_ipl_have_command) {  // the command word
    g_ipl_command = t->immediate_data ? *t->immediate_data : 0;
    g_ipl_have_command = true;
    return true;
  }
  const uint32_t address = (g_ipl_command >> 6) & 0x1FFFFFF;
  if (t->dma) {
    uint8_t* dst = guest(0x80000000 | t->dma_address);
    const bool sram = address >= 0x800004 && address < 0x800004 + sizeof(g_sram);
    const uint32_t sram_off = address - 0x800004;
    if (dst && t->direction == 0) {
      std::memset(dst, 0, t->length);
      if (sram)
        std::memcpy(dst, g_sram + sram_off, std::min<uint32_t>(t->length, sizeof(g_sram) - sram_off));
    } else if (dst && t->direction == 1 && sram) {  // the SDK writing SRAM back
      std::memcpy(g_sram + sram_off, dst, std::min<uint32_t>(t->length, sizeof(g_sram) - sram_off));
    }
  } else if (t->immediate_data && t->direction == 0) {
    *t->immediate_data = address == 0x800000 ? 0 : 0;  // RTC: the epoch
  }
  g_ipl_have_command = false;
  return true;
}

// --- RELs ---------------------------------------------------------------------------------

// The game loads relocatable modules (RELs) into the heap wherever there is
// room; the module holds their code compiled at fixed "linked" addresses. A
// REL is found by its header in RAM, and each section's runtime address maps
// to its linked one (and back), as ModernGekko's StaticRecompCore does.
const ModuleDesc* g_module = nullptr;
struct ActiveRelSection {
  uint32_t linked_start, runtime_start, size;
};
std::vector<ActiveRelSection> g_rel_sections;

void refresh_rel_sections() {
  g_rel_sections.clear();
  for (uint32_t m = 0; m < g_module->num_rel_modules; ++m) {
    const RelModule& rel = g_module->rel_modules[m];
    const uint64_t table_end = uint64_t(rel.section_info_offset) + uint64_t(rel.section_count) * 8;
    for (uint32_t at = 0; uint64_t(at) + table_end <= kRamSize; at += 4) {
      const uint8_t* header = g_ram.data() + at;
      if (rd32(header) != rel.module_id || rd32(header + 0x0C) != rel.section_count ||
          rd32(header + 0x10) != rel.section_info_offset || rd32(header + 0x1C) != rel.version)
        continue;
      std::vector<ActiveRelSection> found;
      bool valid = true;
      for (uint32_t i = 0; i < rel.num_sections && valid; ++i) {
        const RelSection& section = rel.sections[i];
        const uint8_t* entry = header + rel.section_info_offset + section.section_index * 8;
        uint32_t start = rd32(entry) & ~1u;
        if (rd32(entry + 4) != section.size || start == 0) {
          valid = false;
          break;
        }
        if (start < rel.file_size)
          start += 0x80000000u + at;
        else if (start < kRamSize)
          start |= 0x80000000u;
        if (start < 0x80000000u || uint64_t(start - 0x80000000u) + section.size > kRamSize)
          valid = false;
        else
          found.push_back({section.linked_start, start, section.size});
      }
      if (valid) {
        g_rel_sections.insert(g_rel_sections.end(), found.begin(), found.end());
        break;
      }
    }
  }
}

// Runtime -> linked, for a PC the module has no code at directly.
bool rel_linked(uint32_t runtime, uint32_t* linked) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (const ActiveRelSection& s : g_rel_sections)
      if (runtime >= s.runtime_start && runtime - s.runtime_start < s.size) {
        *linked = s.linked_start + (runtime - s.runtime_start);
        return true;
      }
    if (attempt == 0)
      refresh_rel_sections();
  }
  return false;
}

// Linked -> runtime, for a PC or an address the REL's code produced.
uint32_t rel_runtime(uint32_t address) {
  for (const ActiveRelSection& s : g_rel_sections)
    if (address >= s.linked_start && address - s.linked_start < s.size)
      return s.runtime_start + (address - s.linked_start);
  return address;
}

// --- CPU callbacks ------------------------------------------------------------------

uint64_t cb_read(CPUState*, uint32_t ea, uint8_t size) {
  if ((ea & 0xFE000000) == 0xCC000000 || (ea & 0xFE000000) == 0x0C000000)
    return mmio_read(ea, size);
  // A REL's linked address, or a physical one (exception handlers run with
  // data translation off): both are RAM.
  uint32_t runtime = rel_runtime(ea);
  if (runtime == ea && ea < kRamSize)
    runtime = 0x80000000 | ea;
  if (runtime != ea)
    if (const uint8_t* p = guest(runtime))
      return size == 1 ? p[0] : size == 2 ? rd16(p) : size == 4 ? rd32(p) : (uint64_t(rd32(p)) << 32) | rd32(p + 4);
  if ((ea & 0xFF000000) == 0xC8000000)  // EFB peeks
    return 0;
  ++g_stat_unknown_reads;
  warn_once("read outside memory", ea);
  return 0;
}

void cb_write(CPUState*, uint32_t ea, uint64_t value, uint8_t size) {
  if ((ea & 0xFE000000) == 0xCC000000 || (ea & 0xFE000000) == 0x0C000000) {
    mmio_write(ea, value, size);
    return;
  }
  uint32_t runtime = rel_runtime(ea);
  if (runtime == ea && ea < kRamSize)
    runtime = 0x80000000 | ea;
  if (runtime != ea)
    if (uint8_t* p = guest(runtime)) {
      for (int i = size - 1; i >= 0; --i)
        p[size - 1 - i] = uint8_t(value >> (8 * i));
      return;
    }
  if ((ea & 0xFF000000) == 0xC8000000)  // EFB pokes
    return;
  ++g_stat_unknown_writes;
  warn_once("write outside memory", ea);
}

uint32_t cb_read32(CPUState* cpu, uint32_t ea, uint8_t) { return uint32_t(cb_read(cpu, ea, 4)); }
void cb_write32(CPUState* cpu, uint32_t ea, uint32_t value, uint8_t) { cb_write(cpu, ea, value, 4); }

void* cb_pointer(CPUState*, uint32_t ea, uint32_t size) {
#if defined(__linux__) && defined(__x86_64__)
  if (ea == 0xFFFFF000 && size == 0)
    return g_module_alias_safe ? g_ram.fastmem_view() : nullptr;
#endif
  if (ea == 0xE0000000)
    return g_l1;
  // The Colosseum glyph hook's slot query (GXRuntime native_sdk.c): HD text's
  // slot placement, asked once and called for every glyph.
  if (ea == 0xFFFFE000 && size == 0)
    return reinterpret_cast<void*>(&native_text_place_slot);
  return nullptr;  // no fast gather pipe; its stores keep using cb_write
}

uint32_t timebase_lo() { return uint32_t(g_cpu.timebase); }

uint32_t cb_spr_read(CPUState* cpu, uint16_t spr, uint32_t) {
  switch (spr) {
  case 22: return g_dec;                          // DEC
  case 268: return uint32_t(cpu->timebase);       // TBL
  case 269: return uint32_t(cpu->timebase >> 32); // TBU
  case 920: return cpu->hid2;                     // HID2
  default: return spr < 1024 ? g_spr[spr] : 0;
  }
}

// Locked-cache DMA (DMAU/DMAL): moves 32-byte lines between RAM and the L1.
void locked_cache_dma() {
  const uint32_t dmau = g_spr[922], dmal = g_spr[923];
  const uint32_t mem = (dmau & 0xFFFFFFE0);
  const uint32_t cache = (dmal & 0xFFFFFFE0) - 0xE0000000;
  uint32_t lines = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
  if (lines == 0)
    lines = 128;
  const uint32_t bytes = lines * 32;
  uint8_t* ram = guest(mem);
  if (ram && cache + bytes <= kLockedCacheSize) {
    if (dmal & 0x10)  // load: RAM -> cache
      std::memcpy(g_l1 + cache, ram, bytes);
    else
      std::memcpy(ram, g_l1 + cache, bytes);
  }
  g_spr[923] &= ~0x3u;  // done (trigger and flush bits clear)
}

void cb_spr_write(CPUState* cpu, uint16_t spr, uint32_t value, uint32_t) {
  switch (spr) {
  case 22:  // DEC
    g_dec = value;
    g_dec_pending = false;
    return;
  case 284:  // TBL (write)
    cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value;
    return;
  case 285:  // TBU (write)
    cpu->timebase = (cpu->timebase & 0xFFFFFFFFull) | (uint64_t(value) << 32);
    return;
  case 920:
    cpu->hid2 = value;
    return;
  case 921:  // WPAR: gather pipe target; resetting it empties the pipe
    g_spr[921] = value;
    return;
  case 923:
    g_spr[923] = value;
    if (value & 0x2)
      locked_cache_dma();
    return;
  default:
    if (spr < 1024)
      g_spr[spr] = value;
  }
}

void cb_cache(CPUState*, uint8_t, uint32_t, uint32_t) {}  // no caches to keep coherent

// The module resumes through the dispatcher at the pc this leaves: execute the
// instruction at cia and step past it.
void cb_fallback(CPUState* cpu, uint32_t raw, uint32_t cia) {
  cpu->pc = rel_runtime(cia) + 4;
  const uint32_t op = raw >> 26, xo = (raw >> 1) & 0x3FF;
  if (op == 31) {
    switch (xo) {
    case 54: case 86: case 470: case 982: case 598: case 150:  // dcbst dcbf dcbi icbi sync stwcx?
      return;
    case 339: {  // mfspr
      const uint32_t spr = ((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5);
      cpu->gpr[(raw >> 21) & 0x1F] = cb_spr_read(cpu, uint16_t(spr), cia);
      return;
    }
    case 467: {  // mtspr
      const uint32_t spr = ((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5);
      cb_spr_write(cpu, uint16_t(spr), cpu->gpr[(raw >> 21) & 0x1F], cia);
      return;
    }
    case 371: {  // mftb
      const uint32_t tbr = ((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5);
      cpu->gpr[(raw >> 21) & 0x1F] = tbr == 269 ? uint32_t(cpu->timebase >> 32) : uint32_t(cpu->timebase);
      return;
    }
    }
  }
  if (op == 19 && xo == 150)  // isync
    return;
  warn_once("instruction the module handed back", raw);
}

// --- Exceptions and time --------------------------------------------------------------

constexpr uint32_t kMsrEE = 0x8000;

void take_exception(uint32_t vector) {
  // As the 750CL does: SRR0/1 take the return point and MSR, the handler runs
  // with external interrupts, address translation and the FPU off.
  g_cpu.srr0 = g_cpu.pc;
  g_cpu.srr1 = g_cpu.msr & 0x87C0FFFF;
  g_cpu.msr &= ~0x04EF36u;
  g_cpu.pc = ((g_cpu.msr & 0x40) ? 0xFFF00000 : 0x80000000) | vector;
}

void advance_time(uint64_t cycles) {
  g_cycles += cycles;
  g_tick_remainder += cycles;
  const uint64_t ticks = g_tick_remainder / kCyclesPerTick;
  g_tick_remainder %= kCyclesPerTick;
  if (ticks) {
    g_cpu.timebase += ticks;
    const uint32_t before = g_dec;
    g_dec -= uint32_t(ticks);
    if (int32_t(before) >= 0 && int32_t(g_dec) < 0)  // bit 31 0 -> 1 raises it
      g_dec_pending = true;
  }
}

using Clock = std::chrono::steady_clock;
Clock::time_point g_start;
// After the pause menu closes, the pad reads as idle until everything is let
// go, so the press that closed it does not reach the game.
bool g_hold_input = false;
bool g_scripted_input = false;
// Fields since the game resumed, to show it did not run ahead to catch up.
uint64_t g_resume_field = 0;
Clock::time_point g_resume_time;

// Cocoa video and input sampling belong to main. Scripted runs use the same
// rendezvous, so they also exercise the physical-input threading path.
#ifdef __APPLE__
std::mutex g_input_mutex;
std::condition_variable g_input_cv;
bool g_input_pending = false;
uint64_t g_input_field = 0;
PadInput g_main_thread_pad;
std::atomic<bool> g_close_requested{false};
#endif

void poll_main_thread() {
#ifdef __APPLE__
  {
    std::lock_guard lock(g_input_mutex);
    if (g_input_pending) {
      g_main_thread_pad = pad_input_read(g_input_field);
      g_input_pending = false;
      g_input_cv.notify_one();
    }
  }
  if (g_close_requested.exchange(false)) {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
  }
#endif
}

PadInput read_pad(uint64_t field) {
#ifdef __APPLE__
  std::unique_lock lock(g_input_mutex);
  g_input_field = field;
  g_input_pending = true;
  aurora_native_wake_main_thread();
  g_input_cv.wait(lock, [] { return !g_input_pending; });
  return g_main_thread_pad;
#else
  return pad_input_read(field);
#endif
}

void vi_field() {
  // The module is compiled with ModernGekko's widescreen guest hook
  // (0x800A3930 -> 0x80005300), which scales every projection's aspect by the
  // float at 0x8047E724. That word is BSS the game never writes; left at 0 the
  // projection degenerates and 2D menus come out squashed and mirrored.
  // presentation.cpp keeps it at 1 (4:3) or at the Hor+ multiplier, as
  // ModernGekko does each field.
  static const AspectSetting kSettings[] = {AspectSetting::Standard, AspectSetting::Wide, AspectSetting::Auto};
  presentation_field(g_ram.data(), kRamSize, kSettings[std::clamp(host_ui_aspect(), 0, 2)],
                     host_ui_window_aspect());
  // The front-end's multiplier for the renderer (BP 0x6F), between whole commands.
  uint8_t hor_plus[5];
  if (presentation_take_bp(hor_plus, false))
    aurora_native_feed(hor_plus, sizeof(hor_plus));
  native_text_set_enabled(host_ui_hd_text());
  dol_interrupts_assert_vi_retrace(&g_pi);
  PadInput pad = read_pad(g_fields);
  if (g_hold_input) {
    const auto centered = [](uint8_t v) { return v > 0x80 - 24 && v < 0x80 + 24; };
    if (pad.buttons == 0 && pad.trigger_l < 32 && pad.trigger_r < 32 && centered(pad.stick_x) &&
        centered(pad.stick_y) && centered(pad.c_x) && centered(pad.c_y))
      g_hold_input = false;
    else
      pad = PadInput{};
  }
  g_si.pad[0] = pad;
  si_poll();
  memcard_flush(false);
  sync_device_interrupts();
  ++g_fields;
  // Real time: one field per 1/59.94 s.
  const auto due = g_start + std::chrono::nanoseconds(g_fields * 1001000000ull / 60ull);
  const auto now = Clock::now();
  g_interpolation_catchup = frame_interpolation_catchup(
      g_interpolation_catchup, std::chrono::duration<double>(now - due).count());
  g_interpolation_catchup_fields += g_interpolation_catchup;
  if (due > now)
    std::this_thread::sleep_for(due - now);
  if (g_resume_field && g_fields == g_resume_field + 120) {
    VLOG("standalone: the 120 fields after resuming took %.3f s (2.002 s is real time)\n",
         std::chrono::duration<double>(Clock::now() - g_resume_time).count());
    g_resume_field = 0;
  }
}

HostSettings g_settings;

void save_settings() {
  host_ui_store(&g_settings);
  const bool ok = host_settings_save(g_settings);
  host_ui_saved(ok);
  VLOG("standalone: settings %s %s\n", ok ? "saved to" : "could not be saved to", host_settings_path().c_str());
}

// The pause menu is open: the guest stands still (no fields, no audio DMA),
// the last frame is presented again under the menu, and the clock restarts
// from now when it closes, so nothing runs fast to make up the pause.
// `script_repaints`: a script's MENU, closed after that many repaints.
void pause_loop(uint64_t script_repaints) {
  const auto paused_at = Clock::now();
  const uint64_t pushed = audio_out_frames_pushed();
  VLOG("standalone: paused at field %llu, %llu audio frames pushed\n", (unsigned long long)g_fields,
       (unsigned long long)pushed);
  std::fflush(stdout);
  audio_out_pause(true);
  memcard_flush(true);
  uint64_t repaints = 0;
  auto next = paused_at;
  while (!aurora_native_should_quit()) {
    if (script_repaints && repaints >= script_repaints)
      host_ui_set_menu(false);
    if (!host_ui_menu_open())
      break;
    if (host_ui_take_save_request())
      save_settings();
    // The EFB was cleared by the frame's closing copy: draw the frame again.
    aurora_native_feed(g_last_frame.data(), uint32_t(g_last_frame.size()));
    aurora_native_repaint();
    ++repaints;
    next += std::chrono::nanoseconds(1001000000ull / 60ull);
    const auto now = Clock::now();
    if (next > now)
      std::this_thread::sleep_for(next - now);
    else
      next = now;
  }
  // Put the EFB back as the game left it, partway into its next frame.
  if (repaints)
    aurora_native_feed(g_frame_building.data(), uint32_t(g_frame_building.size()));
  audio_out_pause(false);
  g_start = Clock::now() - std::chrono::nanoseconds(g_fields * 1001000000ull / 60ull);
  g_hold_input = !g_scripted_input;
  g_resume_field = g_fields;
  g_resume_time = Clock::now();
  VLOG("standalone: resumed at field %llu after %.2f s (%llu repaints); %llu audio frames pushed while paused\n",
       (unsigned long long)g_fields, std::chrono::duration<double>(Clock::now() - paused_at).count(),
       (unsigned long long)repaints, (unsigned long long)(audio_out_frames_pushed() - pushed));
  std::fflush(stdout);
}

// --- Boot -------------------------------------------------------------------------------

// The executable's directory, with a trailing separator.
std::string exe_dir() {
  std::string path;
#ifdef _WIN32
  std::vector<wchar_t> buffer(256);
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (!n)
      break;
    if (n < buffer.size()) {
      if (char* utf8 = dol_utf8_from_wide(buffer.data())) {
        path = utf8;
        std::free(utf8);
      }
      break;
    }
    if (buffer.size() >= 32768)
      break;
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0)
    path = buffer.data();
#else
  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer));
  if (n > 0)
    path.assign(buffer, size_t(n));
#endif
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

struct DiscHeader {
  uint32_t dol_offset, fst_offset, fst_size, fst_max;
};

bool read_file_at(FILE* f, uint64_t offset, void* dst, size_t size) {
#ifdef _WIN32
  if (_fseeki64(f, int64_t(offset), SEEK_SET) != 0)
    return false;
#else
  if (fseeko(f, off_t(offset), SEEK_SET) != 0)
    return false;
#endif
  return std::fread(dst, 1, size, f) == size;
}

// What the IPL and the disc's apploader leave behind: the disc header at
// 0x80000000, the DOL loaded, the FST at the top of MEM1 with ArenaHi below it,
// and the OS globals.
bool boot(const char* iso, uint32_t* entry) {
  FILE* f = dol_utf8_fopen(iso, "rb");
  if (!f) {
    std::fprintf(stderr, "standalone: cannot open %s\n", iso);
    return false;
  }
  uint8_t header[0x440];
  if (!read_file_at(f, 0, header, sizeof(header))) {
    std::fclose(f);
    return false;
  }
  const DiscHeader disc{rd32(header + 0x420), rd32(header + 0x424), rd32(header + 0x428), rd32(header + 0x42C)};
  std::memcpy(g_ram.data(), header, 0x20);  // disc ID

  // DOL: 7 text and 11 data sections.
  uint8_t dol[0x100];
  if (!read_file_at(f, disc.dol_offset, dol, sizeof(dol))) {
    std::fclose(f);
    return false;
  }
  for (int i = 0; i < 18; ++i) {
    const uint32_t offset = rd32(dol + 4 * i), address = rd32(dol + 0x48 + 4 * i), size = rd32(dol + 0x90 + 4 * i);
    if (!size)
      continue;
    uint8_t* dst = guest(address);
    if (!dst || (address & 0x1FFFFFFF) + size > kRamSize || !read_file_at(f, disc.dol_offset + offset, dst, size)) {
      std::fprintf(stderr, "standalone: bad DOL section %d\n", i);
      std::fclose(f);
      return false;
    }
  }
  const uint32_t bss = rd32(dol + 0xD8), bss_size = rd32(dol + 0xDC);
  *entry = rd32(dol + 0xE0);
  (void)bss;
  (void)bss_size;  // the game's own startup clears its BSS

  // FST at the top of MEM1, as the apploader puts it.
  const uint32_t fst_address = (0x81800000 - disc.fst_max) & ~0x1Fu;
  if (!read_file_at(f, disc.fst_offset, guest(fst_address), disc.fst_size)) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);

  uint8_t* lo = g_ram.data();
  wr32(lo + 0x20, 0x0D15EA5E);   // boot magic
  wr32(lo + 0x24, 1);            // boot version
  wr32(lo + 0x28, kRamSize);     // physical memory
  wr32(lo + 0x2C, 1);            // retail console
  wr32(lo + 0x30, 0);            // ArenaLo: the linker's
  wr32(lo + 0x34, fst_address);  // ArenaHi: below the FST
  wr32(lo + 0x38, fst_address);  // FST location
  wr32(lo + 0x3C, disc.fst_max); // FST maximum size
  wr32(lo + 0xCC, 0);            // NTSC
  wr32(lo + 0xD0, 0x01000000);   // ARAM size
  wr32(lo + 0xF8, 0x09A7EC80);   // bus clock
  wr32(lo + 0xFC, 0x1CF7C580);   // CPU clock
  wr32(lo + 0x300, 0x4C000064);  // default DSI, FPU and system call handlers: rfi
  wr32(lo + 0x800, 0x4C000064);
  wr32(lo + 0xC00, 0x4C000064);
  wr32(lo + 0x30D8, 0);          // timebase preset (the RTC's epoch)
  wr32(lo + 0x30DC, 0);

  VLOG("standalone: DOL entry 0x%08X, FST 0x%08X (%u bytes)\n", *entry, fst_address, disc.fst_size);
  return true;
}

void init_cpu(uint32_t entry) {
  std::memset(&g_cpu, 0, sizeof(g_cpu));
  g_cpu.ram = g_ram.data();
  g_cpu.ram_size = kRamSize;
  g_cpu.external_read = cb_read;
  g_cpu.external_write = cb_write;
  g_cpu.external_read32 = cb_read32;
  g_cpu.external_write32 = cb_write32;
  g_cpu.external_pointer = cb_pointer;
  g_cpu.instruction_fallback = cb_fallback;
  g_cpu.spr_read = cb_spr_read;
  g_cpu.spr_write = cb_spr_write;
  g_cpu.cache_control = cb_cache;
  g_cpu.msr = 0x00002032;  // FP, IR, DR, RI
  g_cpu.hid2 = 0xE0000000; // paired singles, write gathering, quantized loads
  g_cpu.gpr[1] = 0x81566550;
  g_cpu.gpr[2] = 0x81465CC0;
  g_cpu.gpr[13] = 0x81465320;
  g_cpu.pc = entry;
}

}  // namespace

namespace {

// --- Launch -------------------------------------------------------------------

bool g_dialogs = false;  // message boxes for problems (no arguments, no STANDALONE_NO_DIALOGS)

bool file_exists(const std::string& path) {
  SDL_PathInfo info;
  return !path.empty() && SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

int fail(const std::string& message) {
  std::fprintf(stderr, "standalone: %s\n", message.c_str());
  if (g_dialogs)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Pokemon Colosseum", message.c_str(), nullptr);
  return 1;
}

// A disc image beside the executable: game.iso, game.gcm, else the first
// *.iso or *.gcm.
std::string find_disc_beside_exe() {
  const std::string dir = exe_dir();
  for (const char* name : {"game.iso", "game.gcm"})
    if (file_exists(dir + name))
      return dir + name;
  std::vector<std::string> found;
  for (const char* pattern : {"*.iso", "*.gcm"}) {
    int count = 0;
    if (char** names = SDL_GlobDirectory(dir.c_str(), pattern, SDL_GLOB_CASEINSENSITIVE, &count)) {
      for (int i = 0; i < count; ++i)
        found.push_back(dir + names[i]);
      SDL_free(names);
    }
  }
  std::sort(found.begin(), found.end());
  return found.empty() ? std::string() : found.front();
}

struct DialogResult {
  std::atomic<bool> done{false};
  std::string path;
};

void SDLCALL on_disc_chosen(void* user, const char* const* files, int) {
  auto* result = static_cast<DialogResult*>(user);
  if (files && files[0])
    result->path = files[0];
  result->done = true;
}

// Explains what is missing and offers a file dialog for the disc image.
std::string ask_for_disc() {
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose disc image..."},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
  };
  const std::string message =
      "Pokemon Colosseum needs a disc image of the game (USA, GC6E01) as an .iso or .gcm file.\n\n"
      "Put it next to colosseum-standalone.exe as game.iso, set iso = <path> in\n" +
      host_settings_path() + "\nor choose the file now; it will be remembered.";
  const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "Pokemon Colosseum", message.c_str(),
                               2, buttons, nullptr};
  int pressed = 0;
  if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1)
    return {};
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    return {};
  DialogResult result;
  const SDL_DialogFileFilter filters[] = {{"GameCube disc image (*.iso, *.gcm)", "iso;gcm"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(on_disc_chosen, &result, nullptr, filters, 2, exe_dir().c_str(), false);
  while (!result.done) {
    SDL_Event event;
    SDL_WaitEventTimeout(&event, 20);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return result.path;
}

// Started by double-click (the console is ours alone and nobody reads it):
// drop the console window and log to standalone.log in the settings folder.
void log_to_file_if_unattended() {
#ifdef _WIN32
  DWORD processes[2];
  if (GetConsoleProcessList(processes, 2) != 1 || GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR)
    return;
  const std::string log = host_pref_dir() + "standalone.log";
  FILE* file = dol_utf8_fopen(log.c_str(), "w");
  if (!file)
    return;
  std::fflush(stdout);
  std::fflush(stderr);
  FreeConsole();
  _dup2(_fileno(file), _fileno(stdout));
  _dup2(_fileno(file), _fileno(stderr));
  std::fclose(file);
  std::printf("standalone: started without a console; logging here\n");
  std::fflush(stdout);
#endif
}

// Ends the process once Aurora is up. Its device and threads are never torn
// down (see native.cpp), and the CRT's static teardown would meet them gone
// ("Device lost" and an abort), so the process ends here.
[[noreturn]] void end_process(int code) {
  std::fflush(nullptr);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), UINT(code));
#endif
  std::_Exit(code);
}

}  // namespace

static int standalone_main(int argc, char** argv) {
  std::vector<std::string> positional;
  bool verbose_flag = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verbose" || arg == "-v") {
      verbose_flag = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: colosseum-standalone [--verbose] [<game.iso> [<gGC6E01_recomp.dll>]]\n"
                  "settings: %s\n", host_settings_path().c_str());
      return 0;
    } else {
      positional.push_back(arg);
    }
  }
  const bool player = positional.empty();
  const char* no_dialogs = std::getenv("STANDALONE_NO_DIALOGS");
  g_dialogs = player && !(no_dialogs && *no_dialogs && std::strcmp(no_dialogs, "0") != 0);
  HostSettings& settings = g_settings;
  host_settings_load(&settings);
  const char* verbose_env = std::getenv("STANDALONE_VERBOSE");
  g_verbose = verbose_flag || (verbose_env && *verbose_env ? std::strcmp(verbose_env, "0") != 0 : settings.verbose);
  if (player)
    log_to_file_if_unattended();
  g_scripted_input = !dol_utf8::environment("STANDALONE_INPUT_SCRIPT").empty();

  // The disc image: the argument, the settings, beside the executable, or asked for.
  std::string iso = positional.size() >= 1 ? positional[0] : std::string();
  if (iso.empty() && file_exists(settings.iso))
    iso = settings.iso;
  if (iso.empty())
    iso = find_disc_beside_exe();
  if (iso.empty() && g_dialogs) {
    iso = ask_for_disc();
    if (!iso.empty()) {
      settings.iso = iso;
      host_settings_save(settings);
    }
  }
  if (!positional.empty() && settings.iso.empty())
    settings.iso = iso;  // remembered for launches without arguments
  if (iso.empty())
    return fail("no disc image: pass one, put game.iso next to the executable, or set iso = <path> in " +
                host_settings_path());

  // The module: the argument, the settings, beside the executable, or the build's.
  std::string module_path = positional.size() >= 2 ? positional[1] : std::string();
  if (module_path.empty() && file_exists(settings.module))
    module_path = settings.module;
#if defined(_WIN32)
  const std::string module_name = "gGC6E01_recomp.dll";
#elif defined(__APPLE__)
  const std::string module_name = "gGC6E01_recomp.dylib";
#else
  const std::string module_name = "gGC6E01_recomp.so";
#endif
  for (const std::string& candidate : {exe_dir() + module_name, host_pref_dir() + module_name})
    if (module_path.empty() && file_exists(candidate))
      module_path = candidate;
  if (module_path.empty())
    return fail("the recompiled module " + module_name + " was not found next to the executable or in the settings folder. "
                "Run python scripts/setup-standalone.py <ISO> (see tools/aurora-mirror/SETUP.md), "
                "or set module = <path> in " + host_settings_path());

  const ModuleDesc* module = load_module(module_path.c_str());
  if (!module)
    return fail(module_path + " could not be loaded as the game's module");
  g_module = module;
  VLOG("standalone: module %.6s, %u code ranges, %u RELs\n", module->game_id, module->num_code_ranges,
       module->num_rel_modules);
  for (uint32_t i = 0; i < module->num_code_ranges; ++i)
    VLOG("  code 0x%08X-0x%08X\n", module->code_ranges[i].start, module->code_ranges[i].end);
  for (uint32_t m = 0; m < module->num_rel_modules; ++m)
    VLOG("  REL %u: %u sections, linked at 0x%08X\n", module->rel_modules[m].module_id,
         module->rel_modules[m].num_sections,
         module->rel_modules[m].num_sections ? module->rel_modules[m].sections[0].linked_start : 0);
  if (!dvd_open_image(iso.c_str()))
    return fail(iso + " is not a readable disc image");
  uint32_t entry = 0;
  if (!boot(iso.c_str(), &entry))
    return fail(iso + " does not boot (not a Pokemon Colosseum disc image?)");
  init_cpu(entry);

  dol_interrupts_init(&g_pi);
  dol_di_init(&g_di);
  dol_di_set_command_callback(&g_di, di_command, nullptr);
  dol_di_set_disc_present(&g_di, true);
  dol_exi_init(&g_exi);
  dol_exi_set_transfer_callback(&g_exi, exi_transfer, nullptr);
  {
    const std::string card = dol_utf8::environment("STANDALONE_CARD");
    const std::string card_path = !card.empty()             ? card
                                  : !settings.memcard.empty() ? settings.memcard
                                                              : host_pref_dir() + "MemoryCardA.USA.raw";
    if (memcard_open(card_path, g_sram + kSramFlashId, g_sram[0x12])) {
      dol_exi_set_device_present(&g_exi, 0, true, false);
      sram_set_card_flash_id(memcard_header(), 0);
    }
    sram_fix_checksums();
  }
  pad_input_init();
  pad_input_set_deadzone(settings.deadzone);
  audio_out_init(settings.audio);
  audio_out_set_volume(float(settings.volume) / 100.0f);

  aurora_native_set_verbose(g_verbose);
  aurora_native_configure(unsigned(settings.window_width), unsigned(settings.window_height), settings.vsync);
  host_ui_init(settings);
  aurora_native_configure_texture_pack(settings.texture_pack.c_str());
  aurora_native_set_frame_rate(0, false, 1);
  // Widescreen frames at their own aspect, with bars where the window's shape
  // differs (presentation.cpp decides the aspect each frame).
  aurora_native_set_exact_aspect(true);
  native_text_set_guest_ram(g_ram.data(), kRamSize);
  native_text_set_enabled(settings.hd_text);
  aurora_native_set_text_rasterizer(native_text_rasterize);
#ifdef __APPLE__
  aurora_native_use_main_thread_renderer();
#endif
  if (!aurora_native_init(g_ram.data(), kRamSize))
    return fail("the renderer did not start (no usable GPU device?)");
  host_ui_start();

  auto run_guest = [&] {
  g_start = Clock::now();
#if defined(__linux__) && defined(__x86_64__)
  VLOG("standalone: Linux logical memory view %s\n", g_ram.fastmem_view() ? "reserved" : "unavailable (checked fallback)");
#endif
  std::vector<std::pair<uint32_t, uint32_t>> samples;  // (pc, count), STANDALONE_SAMPLE
  uint64_t dispatches = 0, exceptions = 0, last_report_fields = 0;
  uint64_t idle_waits = 0, idle_cycles_waited = 0;
  const bool idle_wait_enabled = !(std::getenv("STANDALONE_IDLE_WAIT") &&
                                  std::strcmp(std::getenv("STANDALONE_IDLE_WAIT"), "0") == 0);
  const uint64_t limit_fields = std::getenv("STANDALONE_FIELDS") ? std::strtoull(std::getenv("STANDALONE_FIELDS"), nullptr, 0) : 0;
  while (!aurora_native_should_quit()) {
    // Interrupts the guest has enabled, between dispatches.
    if (g_cpu.msr & kMsrEE) {
      if (dol_interrupts_external_pending(&g_pi)) {
        take_exception(0x500);
        ++exceptions;
        ++g_stat_external;
      } else if (g_dec_pending) {
        g_dec_pending = false;
        take_exception(0x900);
        ++exceptions;
        ++g_stat_decrementer;
      }
    }
    static const long trace_budget = std::getenv("STANDALONE_TRACE") ? std::atol(std::getenv("STANDALONE_TRACE")) : 0;
    static long traced = 0;
    if (trace_budget && g_stat_external && traced < trace_budget) {
      ++traced;
      std::printf("  trace pc 0x%08X lr 0x%08X msr 0x%08X r3 0x%08X cause 0x%X\n", g_cpu.pc, g_cpu.lr, g_cpu.msr,
                  g_cpu.gpr[3], dol_interrupts_pi_cause(&g_pi));
    }
    // Bring-up aid: the last dispatch addresses, dumped the first time
    // execution leaves the module's code and the low-memory vectors.
    static uint32_t ring[64][3];
    static uint32_t ring_at = 0;
    static bool ring_dumped = false;
    // This bring-up ring is consumed once. Stop recording after that dump,
    // and do no diagnostic memory traffic during ordinary non-verbose play.
    if (g_verbose && !ring_dumped) {
      ring[ring_at & 63][0] = g_cpu.pc;
      ring[ring_at & 63][1] = g_cpu.lr;
      ring[ring_at & 63][2] = g_cpu.gpr[1];
      ++ring_at;
    }
    if (g_verbose && !ring_dumped && g_cpu.pc >= 0x80003000) {
      bool in_code = false;
      for (uint32_t i = 0; i < module->num_code_ranges; ++i)
        in_code |= g_cpu.pc >= module->code_ranges[i].start && g_cpu.pc < module->code_ranges[i].end;
      if (!in_code) {
        ring_dumped = true;
        std::printf("standalone: left the module's code at 0x%08X; last dispatches (pc lr sp):\n", g_cpu.pc);
        for (uint32_t i = 0; i < 64; ++i) {
          const uint32_t* e = ring[(ring_at + i) & 63];
          std::printf("  0x%08X 0x%08X 0x%08X\n", e[0], e[1], e[2]);
        }
        std::fflush(stdout);
      }
    }
    static const bool sampling = std::getenv("STANDALONE_SAMPLE") != nullptr;
    if (sampling && (dispatches & 63) == 0) {
      auto it = std::find_if(samples.begin(), samples.end(), [](auto& e) { return e.first == g_cpu.pc; });
      if (it != samples.end()) ++it->second; else if (samples.size() < 4096) samples.push_back({g_cpu.pc, 1});
    }
    g_cpu.downcount = 0;
    uint64_t idle_cycles = 0;
    if (idle_wait_enabled && g_cpu.pc == 0x800FEB9C) {
      // AX can post immediate mail; service it before proving the idle state.
      ax_update();
      idle_cycles = background_idle_cycles(g_ram.data(), g_ram.size(),
          {g_cpu.pc, g_cpu.msr, uint32_t(g_cpu.gpr[13]), uint32_t(g_cpu.gpr[3]), uint32_t(g_cpu.gpr[0]),
           g_cycles, g_next_field, g_dsp.ai_dma_due, memcard_next_event(), g_dec, uint32_t(g_tick_remainder),
           dol_interrupts_external_pending(&g_pi), g_dec_pending, !g_dsp.mail_to_cpu.empty()});
    }
    if (!idle_cycles) {
      const uint32_t dispatch_pc = g_cpu.pc;
      module->dispatch(&g_cpu, g_cpu.pc);
      if (g_cpu.downcount == 0 && g_cpu.pc == dispatch_pc && module->num_rel_modules) {
        uint32_t linked;
        if (rel_linked(dispatch_pc, &linked) && linked != dispatch_pc) {
          g_cpu.pc = linked;
          module->dispatch(&g_cpu, linked);
        }
      }
      if (module->num_rel_modules)
        g_cpu.pc = rel_runtime(g_cpu.pc);
      ++dispatches;
      if (g_cpu.exception) {  // the module already redirected to the vector
        g_cpu.exception = 0;
        g_cpu.program_exception = 0;
        ++exceptions;
      }
      // A dispatch that neither moved nor charged anything: the module has no
      // code here. Dump the guest code around it and stop (bring-up aid).
      static uint32_t stall_pc = 0, stall_count = 0;
      static uint64_t interpreted = 0;
      if (g_cpu.downcount == 0 && g_cpu.pc == stall_pc && gekko_interpret(&g_cpu)) {
        ++interpreted;
        g_cpu.downcount = -1;
        stall_count = 0;
        stall_pc = 0;
      } else if (g_cpu.downcount == 0 && g_cpu.pc == stall_pc) {
        if (++stall_count == 1000) {
          std::printf("standalone: stalled at 0x%08X (lr 0x%08X, srr0 0x%08X, srr1 0x%08X, msr 0x%08X)\n",
                      g_cpu.pc, g_cpu.lr, g_cpu.srr0, g_cpu.srr1, g_cpu.msr);
          for (uint32_t a = (g_cpu.pc & ~3u) - 0x20; a < (g_cpu.pc & ~3u) + 0x80; a += 4)
            if (const uint8_t* p = guest(a))
              std::printf("  %08X: %08X\n", a, rd32(p));
          std::fflush(stdout);
          break;
        }
      } else {
        stall_pc = g_cpu.pc;
        stall_count = 0;
      }
    } else {
      ++idle_waits;
      idle_cycles_waited += idle_cycles;
    }
    const int64_t charge = -g_cpu.downcount;
    advance_time(idle_cycles ? idle_cycles : charge > 0 ? uint64_t(charge) : 1);
    ai_dma_tick();
    ax_update();
    if (memcard_tick(g_cycles))
      sync_device_interrupts();
    if (g_cycles >= g_next_field) {
      g_next_field += kCyclesPerField;
      vi_field();
      const PadScriptAction action = pad_input_script_action(g_fields);
      if (action.quit)
        aurora_native_request_quit();
      if (action.close) {
#ifdef __APPLE__
        g_close_requested = true;
        aurora_native_wake_main_thread();
#else
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
#endif
      }
      if (action.menu_repaints)
        host_ui_set_menu(true);
      if (host_ui_menu_open() && !aurora_native_should_quit())
        pause_loop(action.menu_repaints);
      if (g_verbose && g_fields - last_report_fields >= 60) {
        last_report_fields = g_fields;
        std::printf("standalone: %llu fields, %llu frames, %llu draw-dones, pc 0x%08X, %llu dispatches, "
                    "%llu exceptions (%llu ext, %llu dec), PI cause 0x%X mask 0x%X, unknown reads/writes %llu/%llu, bad GX %llu, "
                    "dsp mails in/out %u/%u (queued %zu), AX lists %llu, audio queued %d ms\n",
                    (unsigned long long)g_fields, (unsigned long long)g_stat_frames,
                    (unsigned long long)g_stat_draw_done, g_cpu.pc, (unsigned long long)dispatches,
                    (unsigned long long)exceptions, (unsigned long long)g_stat_external,
                    (unsigned long long)g_stat_decrementer, dol_interrupts_pi_cause(&g_pi),
                    dol_interrupts_pi_mask(&g_pi), (unsigned long long)g_stat_unknown_reads,
                    (unsigned long long)g_stat_unknown_writes, (unsigned long long)g_stat_bad_commands,
                    g_dsp.mails_received, g_dsp.mails_sent, g_dsp.mail_to_cpu.size(),
                    (unsigned long long)ax_command_lists(), audio_out_queued_ms());
        if (!samples.empty()) {
          std::sort(samples.begin(), samples.end(), [](auto& x, auto& y) { return x.second > y.second; });
          std::printf("  samples:");
          for (size_t i = 0; i < samples.size() && i < 12; ++i)
            std::printf(" %08X:%u", samples[i].first, samples[i].second);
          std::printf("\n");
          samples.clear();
        }
        std::fflush(stdout);
      }
      if (limit_fields && g_fields >= limit_fields)
        break;
    }
  }
  memcard_flush(true);
  VLOG("standalone: %llu idle waits, %llu cycles advanced to device deadlines\n",
       (unsigned long long)idle_waits, (unsigned long long)idle_cycles_waited);
  VLOG("standalone: emulation wall %.3f s, nominal guest %.3f s\n",
       std::chrono::duration<double>(Clock::now() - g_start).count(), g_fields * 1.001 / 60.0);
  VLOG("standalone: %llu fields used native video while catching up\n",
       (unsigned long long)g_interpolation_catchup_fields);
  if (!g_scripted_input || std::getenv("STANDALONE_PREF_DIR"))  // the window as it was left
    save_settings();
  VLOG("standalone: %llu fields (%.2f s), audio %llu frames pushed (%.2f s at %u Hz), %llu dropped, %d ms queued\n",
              (unsigned long long)g_fields, g_fields * 1.001 / 60.0, (unsigned long long)audio_out_frames_pushed(),
              double(audio_out_frames_pushed()) / ai_dma_rate(), ai_dma_rate(),
              (unsigned long long)audio_out_frames_dropped(), audio_out_queued_ms());
  };
  aurora_native_run_main_thread([](void* data) { (*static_cast<decltype(run_guest)*>(data))(); },
                                &run_guest, poll_main_thread);
  audio_out_shutdown();
  aurora_native_shutdown();
  VLOG("standalone: exit 0\n");
  end_process(0);
}

#ifdef _WIN32
// The Windows CRT supplies wide argv directly: SDL and all host paths use UTF-8.
int wmain(int argc, wchar_t** wide_argv) {
  std::vector<std::string> arguments;
  arguments.reserve(size_t(argc));
  for (int i = 0; i < argc; ++i) {
    char* value = dol_utf8_from_wide(wide_argv[i]);
    if (!value) {
      std::fputs("standalone: command line is not valid Unicode\n", stderr);
      return 1;
    }
    arguments.emplace_back(value);
    std::free(value);
  }
  std::vector<char*> argv;
  argv.reserve(size_t(argc) + 1);
  for (auto& argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  return standalone_main(argc, argv.data());
}
#else
int main(int argc, char** argv) { return standalone_main(argc, argv); }
#endif
