// aurora-mirror: draws the game's GX command stream with Aurora, beside Dolphin.
//
// moderngekko-run starts it (MODERNGEKKO_AURORA_MIRROR=<this exe>) with its own
// process id. The runner puts every GX command its GPU thread executes into the
// shared ring "moderngekko-aurora-mirror.<pid>" (display lists flattened in
// place, indexed XF loads made immediate), with a marker after each XFB copy.
// Guest RAM is Dolphin's own shared segment "dolphin-emu.<pid>", mapped
// read-only here, so textures, TLUTs and vertex arrays come straight from the
// game's memory.
//
// The stream goes through GXRuntime's Aurora backend, the path its recomps ship
// on: the Dolphin-grounded retail-GX frontend feeding the gxcore render sink.
// By default the runner waits at each frame end until the mirror has drawn the
// frame, so guest memory holds what Dolphin read (VideoCommon/AuroraMirror.cpp).

#include "gxruntime/aurora_backend.h"
#include "gxruntime/gx_recomp.h"

#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#define NOMINMAX
#include <windows.h>

#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
void aurora_backend_present(void);
bool aurora_backend_should_quit(void);
void aurora_backend_gx_write_bytes(const u8* data, u32 size);
void aurora_backend_set_guest_address_resolver(DolPlatformGuestAddressResolverFn resolve, void* user);
}

namespace {
constexpr uint32_t kMagic = 0x4D525541;  // "AURM", VideoCommon/AuroraMirror.cpp
constexpr uint32_t kVersion = 2;  // frame-end records carry the draw aspect
constexpr uint64_t kDataOffset = 4096;
constexpr uint32_t kRamSize = 0x01800000;  // GameCube MEM1

struct Header {
  uint32_t magic;
  uint32_t version;
  uint64_t capacity;
  std::atomic<uint64_t> write;
  std::atomic<uint64_t> read;
  std::atomic<uint64_t> frames;
  std::atomic<uint64_t> dropped_frames;
  std::atomic<uint32_t> desync;
  uint32_t producer_pid;
};

const uint8_t* g_ram = nullptr;
FILE* g_log = stderr;

bool resolve(void*, u32 address, u32 size, DolGuestAddressSpace, DolGuestResourceKind, const void** data,
             u32* available) {
  // Callers pass physical (CP array bases, texture and copy addresses) or
  // cached/uncached virtual addresses. The GPU decodes 25 address bits, so
  // register fields can carry junk above them (texture/TLUT bases do); the
  // mask covers all of these.
  const u32 physical = address & 0x01FFFFFFu;
  if (!g_ram || physical >= kRamSize || size > kRamSize - physical) {
    static int s_logged = 0;
    if (s_logged++ < 8)
      std::fprintf(g_log, "aurora-mirror: unresolved address 0x%08X size %u\n", address, size);
    return false;
  }
  *data = g_ram + physical;
  *available = kRamSize - physical;
  return size <= kRamSize - physical;
}

void* map(const std::string& name, uint64_t size, DWORD access) {
  for (int i = 0; i < 200; ++i) {
    if (HANDLE h = OpenFileMappingA(access, FALSE, name.c_str())) {
      if (void* view = MapViewOfFile(h, access, 0, 0, size))
        return view;
    }
    Sleep(50);
  }
  return nullptr;
}

// AURORA_MIRROR_SNAP=<dir>: every AURORA_MIRROR_SNAP_EVERY frames (default
// 300), the frame Aurora presents, as <dir>/aurora-<frame>.ppm.
const char* snap_dir = std::getenv("AURORA_MIRROR_SNAP");
uint64_t snap_every =
    snap_dir ? (std::getenv("AURORA_MIRROR_SNAP_EVERY") ? std::strtoull(std::getenv("AURORA_MIRROR_SNAP_EVERY"), nullptr, 10)
                                                         : 300)
             : 0;

void save_snapshot(uint64_t frame) {
  const uint8_t* rgba = nullptr;
  u32 width = 0, height = 0;
  bool got = false;
  for (int attempt = 0; attempt < 4000 && !got; ++attempt) {
    got = aurora_take_framebuffer_readback(&rgba, &width, &height);
    if (!got) {
      aurora_pump_framebuffer_readback();
      Sleep(0);
    }
  }
  if (!got)
    return;
  const std::string path = std::string(snap_dir) + "/aurora-" + std::to_string(frame) + ".ppm";
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (uint32_t i = 0; i < width * height; ++i)
      std::fwrite(rgba + i * 4, 1, 3, f);
    std::fclose(f);
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(g_log, "usage: aurora-mirror <moderngekko-run pid>\n");
    return 2;
  }
  const unsigned long pid = std::strtoul(argv[1], nullptr, 10);
  // Started by the runner with no console of its own: log to a file.
  if (const char* temp = std::getenv("TEMP")) {
    const std::string log = std::string(temp) + "/aurora-mirror." + std::to_string(pid) + ".log";
    if (FILE* f = std::fopen(log.c_str(), "w")) {
      std::setvbuf(f, nullptr, _IONBF, 0);
      g_log = f;
      // GXRuntime's backend reports on stderr.
      _dup2(_fileno(f), _fileno(stderr));
      std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
  }
  const std::string ring_name = "moderngekko-aurora-mirror." + std::to_string(pid);
  auto* header = static_cast<Header*>(map(ring_name, kDataOffset, FILE_MAP_ALL_ACCESS));
  if (!header || header->magic != kMagic || header->version != kVersion) {
    std::fprintf(g_log, "aurora-mirror: no ring for process %lu\n", pid);
    return 1;
  }
  const uint64_t capacity = header->capacity;
  auto* base = static_cast<uint8_t*>(map(ring_name, kDataOffset + capacity, FILE_MAP_ALL_ACCESS));
  header = reinterpret_cast<Header*>(base);
  const uint8_t* ring = base + kDataOffset;
  g_ram = static_cast<const uint8_t*>(map("dolphin-emu." + std::to_string(pid), kRamSize, FILE_MAP_READ));
  if (!g_ram) {
    std::fprintf(g_log, "aurora-mirror: cannot map guest RAM of process %lu\n", pid);
    return 1;
  }
  HANDLE producer = OpenProcess(SYNCHRONIZE, FALSE, pid);
  HANDLE data_event = OpenEventA(SYNCHRONIZE, FALSE, (ring_name + ".data").c_str());
  HANDLE done_event = OpenEventA(EVENT_MODIFY_STATE, FALSE, (ring_name + ".done").c_str());

  aurora_backend_set_guest_address_resolver(&resolve, nullptr);
  // The game's widescreen patches lay out the frame for the runner's window
  // shape and Dolphin stretches the EFB to it, so the mirror's window needs
  // the same shape (AURORA_MIRROR_SIZE=<w>x<h>, default 16:9).
  unsigned window_width = 1600, window_height = 900;
  if (const char* size = std::getenv("AURORA_MIRROR_SIZE"))
    std::sscanf(size, "%ux%u", &window_width, &window_height);
  const AuroraBackendConfig config = {
      .app_name = "Colosseum (Aurora mirror)",
      .window_width = window_width,
      .window_height = window_height,
      .vsync = false,
      .allow_texture_dumps = false,
      .info_logging = false,
      .graphics_logging = false,
      .force_untextured = false,
  };
  if (!dol_aurora_initialize(argc, argv, &config)) {
    std::fprintf(g_log, "aurora-mirror: Aurora initialization failed\n");
    return 1;
  }
  // Initialization resets the frontend; give it the resolver again.
  aurora_backend_set_guest_address_resolver(&resolve, nullptr);
  // The SDK baseline a booting game sets before its first frame; the stream
  // rewrites what the game uses (dolgx_replay does the same).
  static std::vector<uint8_t> gx_fifo(64u * 1024u);
  GXInit(gx_fifo.data(), static_cast<u32>(gx_fifo.size()));
  GXFlush();
  std::fprintf(g_log, "aurora-mirror: started\n");

  auto read = [&](uint64_t pos, void* dst, uint64_t size) {
    const uint64_t at = pos % capacity;
    const uint64_t first = std::min(size, capacity - at);
    std::memcpy(dst, ring + at, first);
    std::memcpy(static_cast<uint8_t*>(dst) + first, ring, size - first);
  };

  std::vector<uint8_t> payload;
  auto current_policy = AURORA_VIEWPORT_FIT;  // Aurora's default
  bool warned_desync = false;
  uint64_t drawn = 0;
  // Where the mirror's time goes between reports: feeding the backend,
  // presenting, waiting for the runner; and the stream bytes it fed.
  using Clock = std::chrono::steady_clock;
  auto last_report = Clock::now();
  Clock::duration feed_time{}, present_time{}, idle_time{};
  uint64_t fed_bytes = 0, frames_since = 0;
  while (!aurora_backend_should_quit()) {
    if (producer && WaitForSingleObject(producer, 0) == WAIT_OBJECT_0)
      break;
    uint64_t pos = header->read.load(std::memory_order_relaxed);
    const uint64_t end = header->write.load(std::memory_order_acquire);
    if (pos == end) {
      const auto idle_start = Clock::now();
      if (data_event)
        WaitForSingleObject(data_event, 5);
      else
        Sleep(1);
      idle_time += Clock::now() - idle_start;
      continue;
    }
    while (pos < end) {
      uint32_t head[2];
      read(pos, head, sizeof(head));
      payload.resize(head[1]);
      if (head[1])
        read(pos + 8, payload.data(), head[1]);
      pos += (8 + uint64_t(head[1]) + 7) & ~uint64_t(7);
      if (head[0] == 1) {
        const auto feed_start = Clock::now();
        aurora_backend_gx_write_bytes(payload.data(), head[1]);
        feed_time += Clock::now() - feed_start;
        fed_bytes += head[1];
      } else if (head[0] == 2) {
        // Present as Dolphin does this frame: menus at 4:3 with bars (fit),
        // widescreen gameplay stretched to the window (the game's widescreen
        // patches lay it out for the window shape).
        const auto present_start = Clock::now();
        float aspect = 0.0f;
        if (head[1] >= sizeof(aspect))
          std::memcpy(&aspect, payload.data(), sizeof(aspect));
        const auto policy = aspect > 0.0f && aspect < 1.5f ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH;
        if (policy != current_policy) {
          AuroraSetViewportPolicy(policy);
          current_policy = policy;
        }
        const bool snap = snap_every && drawn % snap_every == 0;
        if (snap)
          aurora_request_framebuffer_readback();
        aurora_backend_present();
        if (snap)
          save_snapshot(drawn);
        present_time += Clock::now() - present_start;
        ++drawn;
        ++frames_since;
      }
    }
    header->read.store(pos, std::memory_order_release);
    if (done_event)
      SetEvent(done_event);

    if (!warned_desync && header->desync.load(std::memory_order_relaxed)) {
      warned_desync = true;
      std::fprintf(g_log, "aurora-mirror: frames were dropped; GX state may be wrong from here\n");
    }
    const auto now = Clock::now();
    if (now - last_report > std::chrono::seconds(5)) {
      const auto per_frame = [&](Clock::duration d) {
        return frames_since ? std::chrono::duration<double, std::milli>(d).count() / frames_since : 0.0;
      };
      std::fprintf(g_log,
                   "aurora-mirror: drew %llu frames, game %llu, dropped %llu; per frame: feed %.2f ms, "
                   "present %.2f ms, idle %.2f ms, %.0f KiB\n",
                   static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(header->frames.load()),
                   static_cast<unsigned long long>(header->dropped_frames.load()), per_frame(feed_time),
                   per_frame(present_time), per_frame(idle_time),
                   frames_since ? fed_bytes / 1024.0 / frames_since : 0.0);
      feed_time = present_time = idle_time = {};
      fed_bytes = frames_since = 0;
      last_report = now;
    }
  }
  dol_aurora_shutdown();
  return 0;
}
