// aurora-native: GXRuntime's Aurora backend as the game's renderer, in the
// runner's process.
//
// The runner loads this DLL when MODERNGEKKO_AURORA_NATIVE names it and runs
// Dolphin headless on its Null video backend: Dolphin still decodes the FIFO and raises the
// PE finish and VI interrupts the game waits on (docs/gx-census-plan.md, phase
// 4), but draws nothing. Its GPU thread hands every GX command to
// aurora_native_feed (display lists flattened, indexed XF loads made immediate;
// VideoCommon/AuroraMirror.cpp), calls aurora_native_frame_end at each XFB copy
// and aurora_native_sync at each GXDrawDone, just before the finish interrupt.
//
// Rendering runs on a thread of its own, which owns Aurora and its window, so
// it overlaps the game's logic instead of following it. The game waits only
// on GXDrawDone, and guest RAM holds still while it does, so at each sync the
// GPU thread copies MEM1 into a snapshot slot and queues the commands gathered
// since the last sync with it; the render thread reads textures and vertex
// arrays from that snapshot. With every slot in use the GPU thread waits, so
// rendering never falls more than kSlots batches behind.
// AURORA_NATIVE_ASYNC=0 renders on the GPU thread instead, against live RAM.
// The standalone Cocoa host inverts ownership: this queue is consumed on main
// while the guest callback runs on a worker, preserving SDL video affinity.
//
// The same path as tools/aurora-mirror (main.cpp), without the second process.

#include "frame_rate.h"
#include "gxruntime/utf8_file.h"
#include "presentation.h"
#include "gxruntime/aurora_backend.h"
#include "gxruntime/gx_recomp.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define AURORA_NATIVE_SSE2 1
#include <emmintrin.h>
#endif

extern "C" void aurora_backend_set_frame_interpolation(uint32_t layers, uint32_t fields);

#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#define AURORA_NATIVE_API extern "C" __declspec(dllexport)
#else
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <string>
#define AURORA_NATIVE_API extern "C" __attribute__((visibility("default")))
#if defined(__GLIBC__)
void end_before_teardown(int status, void*);  // beside DllMain's role
#else
void end_before_teardown();
#endif
#endif

extern "C" {
void aurora_backend_present(void);
bool aurora_backend_should_quit(void);
void aurora_backend_gx_write_bytes(const u8* data, u32 size);
void aurora_backend_set_guest_address_resolver(DolPlatformGuestAddressResolverFn resolve, void* user);
}

namespace {
using Clock = std::chrono::steady_clock;

// Status lines on stderr; the standalone host turns them off unless asked
// (aurora_native_set_verbose).
bool g_verbose = true;
#define NATIVE_LOG(...)                  \
  do {                                   \
    if (g_verbose)                       \
      std::fprintf(stderr, __VA_ARGS__); \
  } while (0)

// Window size and vsync from the host (aurora_native_configure);
// AURORA_NATIVE_SIZE still wins.
unsigned g_config_width = 1920, g_config_height = 1080;
bool g_config_vsync = false;

// The host's overlay (aurora_native_set_overlay): `draw` runs on the render
// thread just before each present, `snap` may paint over a saved snapshot.
void (*g_overlay_draw)(bool imgui_frame) = nullptr;
void (*g_overlay_snap)(uint8_t* rgb, uint32_t width, uint32_t height) = nullptr;

// Guest RAM the render side reads: the live MEM1 in synchronous mode, the
// current batch's snapshot in asynchronous mode.
const uint8_t* g_ram = nullptr;
uint32_t g_ram_size = 0;
const uint8_t* g_live_ram = nullptr;

// Asynchronous mode copies only the guest pages the renderer reads: every
// page a resolve touches is marked needed (render thread), and each snapshot
// takes the needed pages (GPU thread). A page the current snapshot lacks is
// copied from live RAM on first use, as synchronous mode reads everything.
constexpr uint32_t kPageShift = 12;
std::unique_ptr<std::atomic<uint8_t>[]> g_page_needed;
uint8_t* g_snapshot_valid = nullptr;  // the current batch's snapshot: page copied?
uint64_t g_late_pages = 0;            // render thread

void ensure_pages(uint32_t physical, uint32_t size) {
  if (!g_snapshot_valid || size == 0)
    return;
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = (physical + size - 1) >> kPageShift;
  for (uint32_t page = first; page <= last; ++page) {
    g_page_needed[page].store(1, std::memory_order_relaxed);
    if (!g_snapshot_valid[page]) {
      const size_t at = static_cast<size_t>(page) << kPageShift;
      std::memcpy(const_cast<uint8_t*>(g_ram) + at, g_live_ram + at, size_t(1) << kPageShift);
      g_snapshot_valid[page] = 1;
      ++g_late_pages;
    }
  }
}

bool resolve(void*, u32 address, u32 size, DolGuestAddressSpace, DolGuestResourceKind, const void** data,
             u32* available) {
  // Physical, cached or uncached addresses; the GPU decodes 25 address bits
  // (texture and TLUT base fields carry junk above them).
  const u32 physical = address & 0x01FFFFFFu;
  if (!g_ram || physical >= g_ram_size || size > g_ram_size - physical)
    return false;
  ensure_pages(physical, size);
  *data = g_ram + physical;
  *available = g_ram_size - physical;
  return true;
}

// AURORA_NATIVE_SNAP=<dir>: every AURORA_NATIVE_SNAP_EVERY frames (default
// 300), the presented frame as <dir>/aurora-<frame>.ppm.
float g_naming_snapshot_aspect = 0.0f;  // render thread; zero outside naming
const std::string g_snap_path = dol_utf8::environment("AURORA_NATIVE_SNAP");
const char* g_snap_dir = g_snap_path.empty() ? nullptr : g_snap_path.c_str();
uint64_t g_snap_every =
    g_snap_dir ? (std::getenv("AURORA_NATIVE_SNAP_EVERY") ? std::strtoull(std::getenv("AURORA_NATIVE_SNAP_EVERY"),
                                                                          nullptr, 10)
                                                          : 300)
               : 0;
// AURORA_NATIVE_SNAP_FROM: no snapshots before this frame.
const uint64_t g_snap_from =
    std::getenv("AURORA_NATIVE_SNAP_FROM") ? std::strtoull(std::getenv("AURORA_NATIVE_SNAP_FROM"), nullptr, 10) : 0;

void save_snapshot(uint64_t frame) {
  const uint8_t* rgba = nullptr;
  u32 width = 0, height = 0;
  bool got = false;
  // A zero timeout permits deterministic fail-closed verification. Never let
  // an override extend the normal bounded wait.
  const auto timeout_ms = [] {
    const char* value = std::getenv("AURORA_NATIVE_SNAP_TIMEOUT_MS");
    if (!value)
      return 2000ull;
    char* end = nullptr;
    const auto requested = std::strtoull(value, &end, 10);
    return end != value && *end == 0 ? std::min(requested, 2000ull) : 2000ull;
  }();
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!got && Clock::now() < deadline) {
    got = aurora_take_framebuffer_readback(&rgba, &width, &height);
    if (!got) {
      aurora_pump_framebuffer_readback();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!got) {
    // The in-flight result may complete later. Stop this run's capture stream
    // rather than ever assigning that old image another frame's filename.
    g_snap_every = 0;
    std::fprintf(stderr, "aurora-native: snapshot %llu timed out; further captures disabled for this run\n",
                 static_cast<unsigned long long>(frame));
    return;
  }
  char path[1024];
  std::snprintf(path, sizeof(path), "%s/aurora-%llu.ppm", g_snap_dir, static_cast<unsigned long long>(frame));
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (uint32_t i = 0; i < width * height; ++i)
    std::memcpy(&rgb[size_t(i) * 3], rgba + size_t(i) * 4, 3);
  if (presentation_compose_naming_readback(rgb, width, height, g_naming_snapshot_aspect))
    std::fprintf(stderr, "aurora-native: snapshot %llu is composed renderer readback with naming margins\n",
                 static_cast<unsigned long long>(frame));
  if (g_overlay_snap)
    g_overlay_snap(rgb.data(), width, height);
  if (FILE* f = dol_utf8_fopen(path, "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
  }
}

// ---- Thread placement ----------------------------------------------------------

// The emulation threads ("CPU thread", "Video thread") get the processors of
// the largest L3 minus the core holding processor 0, and on a CPU with more
// than one L3 (a dual-CCD Ryzen) every other thread of the process (this
// DLL's, Dawn's, the driver's, audio) defaults to the other L3s:
// - rendering then does not evict the game's working set (9950X3D boundary
//   savestate: 136 -> 155 FPS);
// - core 0 takes most device interrupts and DPCs, the GPU's among them; runs
//   whose CPU thread settled there lost ~9% of its time to them (144 instead
//   of 158 FPS, in about half of all runs);
// - on Windows the CPU thread has one core of it to itself: the Video thread
//   on its SMT sibling cost it ~3% (battle savestate, 2026-10-30).
// Windows: CPU Sets, not affinity: placement preferences the scheduler honours
// without the hard subset rule. Linux: affinity (sched_setaffinity), with no
// process default: every other thread present is moved, and threads they start
// later inherit it. AURORA_NATIVE_PLACE=0 turns it off.
bool placement_disabled() {
  const char* env = std::getenv("AURORA_NATIVE_PLACE");
  return env && env[0] == '0';
}

#ifdef _WIN32
uint64_t thread_cpu_ns() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    return 0;
  return (((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
          ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime)) *
         100;
}

// CPU Set ids; `other` is empty with one L3.
struct Placement {
  std::vector<ULONG> cpu, video, other;
};

// The group-0 processor masks of one relationship (L3 caches or cores).
std::vector<std::pair<KAFFINITY, DWORD>> processor_masks(LOGICAL_PROCESSOR_RELATIONSHIP relation) {
  std::vector<std::pair<KAFFINITY, DWORD>> masks;  // mask, L3 size (caches)
  DWORD length = 0;
  GetLogicalProcessorInformationEx(relation, nullptr, &length);
  std::vector<uint8_t> info(length);
  if (!GetLogicalProcessorInformationEx(relation, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                                                      info.data()),
                                       &length))
    return masks;
  for (DWORD at = 0; at < length;) {
    const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(info.data() + at);
    if (relation == RelationCache && entry->Cache.Level == 3 && entry->Cache.GroupMask.Group == 0)
      masks.emplace_back(entry->Cache.GroupMask.Mask, entry->Cache.CacheSize);
    else if (relation == RelationProcessorCore && entry->Processor.GroupMask[0].Group == 0)
      masks.emplace_back(entry->Processor.GroupMask[0].Mask, 0);
    at += entry->Size;
  }
  return masks;
}

Placement find_placement() {
  Placement out;
  if (placement_disabled())
    return out;
  KAFFINITY biggest = 0, all = 0;
  DWORD biggest_size = 0;
  for (const auto& [mask, size] : processor_masks(RelationCache)) {
    all |= mask;
    // Ties (no V-cache die) keep the first, the one holding processor 0.
    if (size > biggest_size) {
      biggest_size = size;
      biggest = mask;
    }
  }
  KAFFINITY core0 = 0, cpu_core = 0;
  int cores = 0;
  for (const auto& [mask, size] : processor_masks(RelationProcessorCore)) {
    ++cores;
    if (mask & 1)
      core0 = mask;
    else if ((mask & biggest) == mask && (cpu_core == 0 || mask < cpu_core))
      cpu_core = mask;
  }
  if (biggest == 0 || cores < 4 || cpu_core == 0)
    return out;
  const KAFFINITY emulation = biggest & ~core0;
  const KAFFINITY other = biggest == all ? 0 : all & ~biggest;
  ULONG sets_length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &sets_length, GetCurrentProcess(), 0);
  std::vector<uint8_t> sets(sets_length);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), sets_length,
                                  &sets_length, GetCurrentProcess(), 0))
    return out;
  for (ULONG at = 0; at < sets_length;) {
    const auto* set = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(sets.data() + at);
    if (set->Type == CpuSetInformation && set->CpuSet.Group == 0) {
      const KAFFINITY bit = KAFFINITY(1) << set->CpuSet.LogicalProcessorIndex;
      if (cpu_core & bit)
        out.cpu.push_back(set->CpuSet.Id);
      else if (emulation & bit)
        out.video.push_back(set->CpuSet.Id);
      else if (other & bit)
        out.other.push_back(set->CpuSet.Id);
    }
    at += set->Size;
  }
  if (out.cpu.empty() || out.video.empty())
    out = {};
  return out;
}

// Each call reschedules the process's threads (a ~100 ms hitch when it ran at
// every status report), so this sets the process default once and stops
// looking once both emulation threads are placed.
void place_threads() {
  static const Placement placement = find_placement();
  static bool defaulted = false, done = false;
  if (placement.cpu.empty() || done)
    return;
  if (!defaulted) {
    if (!placement.other.empty())
      SetProcessDefaultCpuSets(GetCurrentProcess(), placement.other.data(),
                               static_cast<ULONG>(placement.other.size()));
    defaulted = true;
  }
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
  int placed = 0;
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId())
      continue;
    const HANDLE thread =
        OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread)
      continue;
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &name)) && name) {
      const std::vector<ULONG>* sets = std::wcscmp(name, L"CPU thread") == 0     ? &placement.cpu
                                       : std::wcscmp(name, L"Video thread") == 0 ? &placement.video
                                                                                 : nullptr;
      if (sets) {
        SetThreadSelectedCpuSets(thread, sets->data(), static_cast<ULONG>(sets->size()));
        ++placed;
      }
      LocalFree(name);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  if (placed == 2) {
    done = true;
    NATIVE_LOG(
                 "aurora-native: CPU thread on %zu processors (one core of the largest L3), Video thread on "
                 "%zu (the rest of it but core 0), the rest on %zu\n",
                 placement.cpu.size(), placement.video.size(), placement.other.size());
  }
}
#else
uint64_t thread_cpu_ns() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

#if defined(__linux__)
std::string read_line(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// A sysfs CPU list ("0-5,12-17") as a set.
cpu_set_t parse_cpu_list(const std::string& list) {
  cpu_set_t set;
  CPU_ZERO(&set);
  size_t at = 0;
  while (at < list.size()) {
    const size_t comma = list.find(',', at);
    const std::string part = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    const size_t dash = part.find('-');
    const int first = std::atoi(part.c_str());
    const int last = dash == std::string::npos ? first : std::atoi(part.c_str() + dash + 1);
    for (int cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu)
      CPU_SET(cpu, &set);
    if (comma == std::string::npos)
      break;
    at = comma + 1;
  }
  return set;
}

struct Placement {
  bool valid = false;
  cpu_set_t emulation{}, other{};  // `other` empty with one L3
};

Placement find_placement() {
  Placement out;
  if (placement_disabled())
    return out;
  // L3s from every processor's index3 cache; ties keep the first, the one
  // holding processor 0.
  const int processors = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
  cpu_set_t biggest, all;
  CPU_ZERO(&biggest);
  CPU_ZERO(&all);
  long biggest_size = 0;
  for (int cpu = 0; cpu < processors && cpu < CPU_SETSIZE; ++cpu) {
    const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index3/";
    if (read_line(dir + "level") != "3")
      continue;
    const cpu_set_t shared = parse_cpu_list(read_line(dir + "shared_cpu_list"));
    const long size = std::atol(read_line(dir + "size").c_str());  // "32768K"
    CPU_OR(&all, &all, &shared);
    if (size > biggest_size) {
      biggest_size = size;
      biggest = shared;
    }
  }
  const cpu_set_t core0 = parse_cpu_list(read_line("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list"));
  if (biggest_size == 0 || processors < 8)
    return out;
  CPU_XOR(&out.emulation, &biggest, &core0);
  CPU_AND(&out.emulation, &out.emulation, &biggest);  // biggest minus core 0
  CPU_XOR(&out.other, &all, &biggest);                // the other L3s
  out.valid = CPU_COUNT(&out.emulation) > 0;
  return out;
}

void place_threads() {
  static const Placement placement = find_placement();
  static bool done = false;
  if (!placement.valid || done)
    return;
  const bool split = CPU_COUNT(&placement.other) > 0;
  std::vector<pid_t> emulation, rest;
  if (DIR* tasks = opendir("/proc/self/task")) {
    while (const dirent* entry = readdir(tasks)) {
      if (entry->d_name[0] == '.')
        continue;
      const std::string name = read_line(std::string("/proc/self/task/") + entry->d_name + "/comm");
      (name == "CPU thread" || name == "Video thread" ? emulation : rest).push_back(std::atoi(entry->d_name));
    }
    closedir(tasks);
  }
  if (emulation.size() < 2)
    return;
  for (const pid_t tid : emulation)
    sched_setaffinity(tid, sizeof(cpu_set_t), &placement.emulation);
  if (split) {
    for (const pid_t tid : rest)
      sched_setaffinity(tid, sizeof(cpu_set_t), &placement.other);
  }
  done = true;
  NATIVE_LOG(
               "aurora-native: emulation threads on %d processors (largest L3, not core 0), the rest on %d\n",
               CPU_COUNT(&placement.emulation), CPU_COUNT(&placement.other));
}
#else
// CPU placement through sysfs and sched_setaffinity is Linux-specific.
void place_threads() {}
#endif
#endif

// ---- Rendering (Aurora's thread) -------------------------------------------

bool g_ready = false;
bool g_aurora_up = false;  // initialized, never torn down; see DllMain
// None set yet: Aurora's gx state starts at FIT, but its window does not fit
// the framebuffer until told to, so the first present sets the policy.
int g_policy = -1;
float g_fit_aspect = 0.0f;            // AuroraSetFrameBufferAspect's
std::atomic<bool> g_naming_fill_requested{false};
std::atomic<bool> g_pda_fill_requested{false};
// Outer-margin backdrop: 0 none, 1 naming stripes, 2 PDA scanlines.
int fill_kind() { return g_naming_fill_requested ? 1 : g_pda_fill_requested ? 2 : 0; }
bool g_exact_aspect = false;          // aurora_native_set_exact_aspect
float g_last_aspect = 4.0f / 3.0f;    // of the last frame_end, for repaints (GPU thread)
Clock::time_point g_last_report;
Clock::duration g_feed_time{}, g_present_time{};
uint64_t g_frames = 0, g_frames_since = 0, g_fed_bytes = 0;
// Counted on the GPU thread (ship), reported and reset on the render thread.
std::atomic<int64_t> g_sync_ns{0}, g_sync_wait_ns{0};
std::atomic<uint64_t> g_snapshot_bytes{0};
std::atomic<uint64_t> g_syncs{0};
// AURORA_NATIVE_WATCH_CHECK=1: compare every page the write watch kept (did not recopy) against
// live RAM at the sync, and count the ones that differ; a writer the watch missed shows up here.
const bool g_watch_check = std::getenv("AURORA_NATIVE_WATCH_CHECK") != nullptr;
std::atomic<uint64_t> g_watch_mismatches{0};
// Pages found written while armed (each cost the writer a fault), or armed for the first time.
std::atomic<uint64_t> g_rearmed_pages{0};
bool g_watch_on = false;
std::string g_texture_pack;
std::atomic<unsigned> g_interpolation_layers{0}, g_interpolation_fields{1};

bool start_aurora() {
  aurora_backend_set_guest_address_resolver(&resolve, nullptr);
  unsigned width = g_config_width, height = g_config_height;
  if (const char* size = std::getenv("AURORA_NATIVE_SIZE"))
    std::sscanf(size, "%ux%u", &width, &height);
  const AuroraBackendConfig config = {
      .app_name = "Pokemon Colosseum (Aurora)",
      .window_width = width,
      .window_height = height,
      .vsync = g_config_vsync,
      .allow_texture_dumps = false,
      .info_logging = false,
      .graphics_logging = false,
      .force_untextured = false,
  };
  char arg0[] = "aurora-native";
  char* argv[] = {arg0, nullptr};
  if (!dol_aurora_initialize(1, argv, &config)) {
    std::fprintf(stderr, "aurora-native: Aurora initialization failed\n");
    return false;
  }
  // Initialization resets the frontend; give it the resolver again.
  aurora_backend_set_guest_address_resolver(&resolve, nullptr);
  // The SDK baseline a booting game sets before its first frame; the stream
  // rewrites what the game uses.
  static std::vector<uint8_t> gx_fifo(64u * 1024u);
  GXInit(gx_fifo.data(), static_cast<u32>(gx_fifo.size()));
  GXFlush();
  // AURORA_NATIVE_PRESENT=0: render every frame but never show it, as Dolphin
  // headless does; measures the renderer without a display's present cost
  // (presenting into Xvfb alone took ~4.3 ms a frame).
  if (const char* present = std::getenv("AURORA_NATIVE_PRESENT"); present && present[0] == '0') {
    aurora_set_present_enabled(false);
    std::fprintf(stderr, "aurora-native: rendering without presenting\n");
  }
  if (!g_texture_pack.empty()) {
    const char* pack = g_texture_pack.c_str();
    aurora_backend_set_texture_pack(&pack, 1);
  }
  if (g_interpolation_layers)
    aurora_backend_set_frame_interpolation(g_interpolation_layers, g_interpolation_fields);
  g_last_report = Clock::now();
  g_aurora_up = true;
#if defined(__GLIBC__)
  on_exit(end_before_teardown, nullptr);
#elif !defined(_WIN32)
  std::atexit(end_before_teardown);
#endif
  NATIVE_LOG("aurora-native: started, %ux%u\n", width, height);
  return true;
}

void render_bytes(const uint8_t* data, size_t size) {
  const auto start = Clock::now();
  // The backend takes the stream in pieces of up to 8 KiB.
  constexpr size_t kPiece = 8 * 1024;
  for (size_t at = 0; at < size; at += kPiece)
    aurora_backend_gx_write_bytes(data + at, static_cast<u32>(std::min(kPiece, size - at)));
  g_feed_time += Clock::now() - start;
  g_fed_bytes += size;
}

void render_present(float aspect, int fill) {
  g_naming_snapshot_aspect = 0.0f;
  const ImGuiContext* naming_imgui = ImGui::GetCurrentContext();
  if (fill == 2 && naming_imgui && naming_imgui->WithinFrameScope) {
    // The PDA's own dark-forest background, outside the 4:3 canvas it is
    // authored for.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x > 0 && display.y > 0) {
      const uint32_t width = uint32_t(display.x), height = uint32_t(display.y);
      const auto margins = presentation_naming_margins(width, height);
      if (margins.left || margins.right < width) {
        auto* draw = ImGui::GetBackgroundDrawList();
        const ImU32 background = IM_COL32(3, 20, 14, 255);
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(float(margins.left), display.y), background);
        draw->AddRectFilled(ImVec2(float(margins.right), 0), display, background);
      }
    }
  }
  if (fill == 1 && naming_imgui && naming_imgui->WithinFrameScope) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x > 0 && display.y > 0) {
      const uint32_t width = uint32_t(display.x), height = uint32_t(display.y);
      const auto margins = presentation_naming_margins(width, height);
      if (margins.left || margins.right < width) {
        g_naming_snapshot_aspect = display.x / display.y;
        auto* draw = ImGui::GetBackgroundDrawList();
        for (uint32_t y = 0; y < height;) {
          const uint32_t color = presentation_naming_color(y, height);
          uint32_t end = y + 1;
          while (end < height && presentation_naming_color(end, height) == color) ++end;
          const ImU32 rgba = IM_COL32((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
          draw->AddRectFilled(ImVec2(0, float(y)), ImVec2(float(margins.left), float(end)), rgba);
          draw->AddRectFilled(ImVec2(float(margins.right), float(y)), ImVec2(display.x, float(end)), rgba);
          y = end;
        }
      }
    }
  }
  static bool previous_naming_fill = false;
  const bool margins_active = g_naming_snapshot_aspect > 0;
  if (margins_active != previous_naming_fill) {
    std::fprintf(stderr, "[presentation] naming margins %s, centered 4:3 canvas, window aspect %.3f\n",
                 margins_active ? "on" : "off", g_naming_snapshot_aspect);
    previous_naming_fill = margins_active;
  }
  const auto start = Clock::now();
  // Menus at 4:3 with bars (fit); widescreen gameplay stretched to the window
  // (the game's widescreen patches lay it out for the window shape).
  const auto policy = aspect > 0.0f && aspect < 1.5f ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH;
  // aurora_native_set_exact_aspect: a wide frame at exactly its aspect,
  // letterboxed or pillarboxed in a window of another shape.
  const float fit = g_exact_aspect && policy == AURORA_VIEWPORT_STRETCH ? aspect : 0.0f;
  if (fit != g_fit_aspect) {
    AuroraSetFrameBufferAspect(fit);
    g_fit_aspect = fit;
  }
  if (policy != g_policy) {
    AuroraSetViewportPolicy(policy);
    g_policy = policy;
  }
  const bool snap = g_snap_every && g_frames >= g_snap_from && g_frames % g_snap_every == 0;
  if (g_overlay_draw) {
    // ImGui calls are valid only inside the frame Aurora began (none while
    // the window cannot present, e.g. minimized).
    const ImGuiContext* imgui = ImGui::GetCurrentContext();
    g_overlay_draw(imgui && imgui->WithinFrameScope);
  }
  if (snap) {
    // A timed-out prior request must never acquire the next frame's filename.
    const uint8_t* stale = nullptr;
    u32 stale_width = 0, stale_height = 0;
    aurora_take_framebuffer_readback(&stale, &stale_width, &stale_height);
    aurora_request_framebuffer_readback();
  }
  aurora_backend_present();
  if (snap)
    save_snapshot(g_frames);
  const auto now = Clock::now();
  g_present_time += now - start;
  ++g_frames;
  ++g_frames_since;
  if (g_frames == 60)
    place_threads();
  if (!g_verbose && now - g_last_report > std::chrono::seconds(5)) {
    place_threads();  // until both emulation threads are found
    g_feed_time = g_present_time = {};
    g_fed_bytes = g_frames_since = 0;
    g_late_pages = 0;
    g_last_report = now;
  }
  if (now - g_last_report > std::chrono::seconds(5)) {
    place_threads();  // until both emulation threads are found
    const auto per_frame = [](Clock::duration d) {
      return std::chrono::duration<double, std::milli>(d).count() / static_cast<double>(g_frames_since);
    };
    // CPU time this thread spent (wall-clock feed time also counts the time
    // it sat preempted): what a change to the render path saves.
    static uint64_t s_last_cpu_ns = 0;
    const uint64_t cpu_ns = thread_cpu_ns();
    const double cpu_ms = (cpu_ns - s_last_cpu_ns) / 1e6 / static_cast<double>(g_frames_since);
    s_last_cpu_ns = cpu_ns;
    std::fprintf(stderr,
                 "aurora-native: %llu frames; per frame: cpu %.2f ms, feed %.2f ms, present %.2f ms, %.0f KiB; "
                 "game thread: %.2f syncs, snapshot %.2f ms (%.0f KiB), waited %.2f ms; late pages %.1f\n",
                 static_cast<unsigned long long>(g_frames), cpu_ms, per_frame(g_feed_time), per_frame(g_present_time),
                 g_fed_bytes / 1024.0 / static_cast<double>(g_frames_since),
                 static_cast<double>(g_syncs.exchange(0)) / static_cast<double>(g_frames_since),
                 per_frame(std::chrono::nanoseconds(g_sync_ns.exchange(0))),
                 static_cast<double>(g_snapshot_bytes.exchange(0)) / 1024.0 / static_cast<double>(g_frames_since),
                 per_frame(std::chrono::nanoseconds(g_sync_wait_ns.exchange(0))),
                 static_cast<double>(g_late_pages) / static_cast<double>(g_frames_since));
    if (g_watch_on)
      std::fprintf(stderr, "aurora-native: write watch: %.1f pages re-armed per frame\n",
                   static_cast<double>(g_rearmed_pages.exchange(0)) / static_cast<double>(g_frames_since));
    if (g_watch_check)
      std::fprintf(stderr, "aurora-native: write watch check: %llu kept pages differed from live RAM\n",
                   static_cast<unsigned long long>(g_watch_mismatches.exchange(0)));
    g_feed_time = g_present_time = {};
    g_fed_bytes = g_frames_since = 0;
    g_late_pages = 0;
    g_last_report = now;
  }
}

// ---- Asynchronous mode -------------------------------------------------------

bool g_main_thread_renderer = false;
bool g_async = [] {
#ifdef __APPLE__
  // Cocoa/SDL video initialization must run on the application's main thread.
  return false;
#else
  const char* v = std::getenv("AURORA_NATIVE_ASYNC");
  return !(v && std::strcmp(v, "0") == 0);
#endif
}();

constexpr int kSlots = 3;
// Something to do after `offset` bytes of the batch's commands, in order:
// present a frame, or put back guest memory as the GPU read it at that point
// (a TLUT load: the snapshot is taken later, when the game may have refilled
// the buffer for another load).
struct Event {
  enum Kind : uint8_t { Present, Patch } kind;
  size_t offset;
  float aspect = 0.0f;          // Present
  unsigned interpolation_layers = 0;
  unsigned interpolation_fields = 1;
  int naming_fill = 0;
  uint32_t address = 0;         // Patch: physical guest address
  uint32_t size = 0;            //        byte count
  size_t data = 0;              //        offset into Batch::patch_bytes
};
struct Batch {
  std::vector<uint8_t> bytes;
  std::vector<Event> events;
  std::vector<uint8_t> patch_bytes;
  size_t presents = 0;
  int slot = -1;
};

std::vector<uint8_t> g_snapshots[kSlots];
std::vector<uint8_t> g_snapshot_pages[kSlots];  // per slot: page copied?

// The runner's write watch over guest RAM (aurora_native_write_watch), when it offers one: each
// sync then recopies only the needed pages written since that slot last copied them, instead of
// every page the renderer has ever read (~3.5 MB a sync, of which ~0.3% had changed). Shared with
// Dolphin's VideoCommon/AuroraMirror.cpp.
struct WriteWatch {
  uint32_t version;  // 1
  const std::atomic<uint32_t>* write_gen;  // per page: generation of the last write while armed
  uint32_t (*begin_sync)();                // starts and returns a new generation
  bool (*rearm)(uint32_t page);            // arms the page; true if it was not armed
};
const WriteWatch* g_watch = nullptr;
std::vector<uint32_t> g_slot_gen[kSlots];  // per slot, per page: generation copied at (0: never)
// Per page: whether it was written, one bit per sync, newest in bit 0. A page written every frame
// (~170 of the renderer's pages in battle: buffers rewritten with the same values) would fault on
// the game thread every frame, which costs more than copying it. Written in at least 2 of the last
// 4 syncs (a frame can have two), it stays unarmed and is copied at every sync, and every kProbe
// syncs it is armed again to see whether it has gone quiet.
std::vector<uint8_t> g_page_written;
constexpr uint32_t kProbe = 64;
bool page_hot(uint8_t written) {
  const unsigned recent = written & 0xFu;
  return (recent & (recent - 1)) != 0;  // two or more bits
}
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Batch> g_queue;          // to the render thread
std::vector<int> g_free_slots;      // snapshot slots not in use
std::vector<Batch> g_spare_batches; // batches back from the render thread
bool g_stop = false;
std::thread g_thread;
std::atomic<bool> g_quit_requested{false};

Batch g_batch;  // being gathered on the GPU thread

void render_loop(void (*poll_main_thread)() = nullptr) {
  for (;;) {
    if (poll_main_thread)
      poll_main_thread();
    Batch batch;
    {
      std::unique_lock lock(g_mutex);
      if (poll_main_thread) {
        if (g_queue.empty() && !g_stop)
          g_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
      }
      if (g_queue.empty()) {
        if (g_stop)
          break;
        continue;
      }
      batch = std::move(g_queue.front());
      g_queue.pop_front();
    }
    uint8_t* snapshot = g_snapshots[batch.slot].data();
    g_ram = snapshot;
    g_snapshot_valid = g_snapshot_pages[batch.slot].data();
    size_t at = 0;
    bool drawable = !aurora_backend_should_quit();
    for (const Event& event : batch.events) {
      if (!drawable)
        break;
      render_bytes(batch.bytes.data() + at, event.offset - at);
      at = event.offset;
      if (event.kind == Event::Present) {
        render_present(event.aspect, event.naming_fill);
        drawable = !aurora_backend_should_quit();
        if (!drawable)
          break;  // SDL closed the target; queued draws must only release their slots
        // The just-presented frame may still own logged draws. Change modes
        // only after it has replayed; the next frame starts a fresh pair.
        if (event.interpolation_layers)
          aurora_backend_set_frame_interpolation(event.interpolation_layers, event.interpolation_fields);
      } else {
        ensure_pages(event.address, event.size);  // the rest of the page too
        std::memcpy(snapshot + event.address, batch.patch_bytes.data() + event.data, event.size);
      }
    }
    if (drawable)
      render_bytes(batch.bytes.data() + at, batch.bytes.size() - at);
    if (aurora_backend_should_quit())
      g_quit_requested = true;
    {
      std::lock_guard lock(g_mutex);
      g_free_slots.push_back(batch.slot);
      batch.bytes.clear();
      batch.events.clear();
      batch.patch_bytes.clear();
      batch.presents = 0;
      batch.slot = -1;
      g_spare_batches.push_back(std::move(batch));
    }
    g_cv.notify_all();
  }
  // No dol_aurora_shutdown: inside the runner, tearing Aurora down left Dawn's
  // shader compiler (dxcompiler.dll) asserting at process exit. The process is
  // exiting; stopping this thread is what matters (left running, it met a
  // device destroyed by exit and aborted).
}

void render_thread(std::promise<bool>* started) {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"Aurora render");
#elif defined(__APPLE__)
  pthread_setname_np("Aurora render");
#else
  pthread_setname_np(pthread_self(), "Aurora render");
#endif
  const bool ok = start_aurora();
  started->set_value(ok);
  if (!ok)
    return;
  render_loop();
}

// Copy with non-temporal stores: the snapshot is read by the render thread
// later, and passing 24 MiB through the cache would push out the emulation
// thread's working set, which is what the game's speed rests on.
void copy_streaming(uint8_t* dst, const uint8_t* src, size_t size) {
  size_t at = 0;
#if defined(AURORA_NATIVE_SSE2)
  if ((reinterpret_cast<uintptr_t>(dst) & 15) == 0 && (reinterpret_cast<uintptr_t>(src) & 15) == 0) {
    for (; at + 64 <= size; at += 64) {
      const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(src + at));
      const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(src + at + 16));
      const __m128i c = _mm_load_si128(reinterpret_cast<const __m128i*>(src + at + 32));
      const __m128i d = _mm_load_si128(reinterpret_cast<const __m128i*>(src + at + 48));
      _mm_stream_si128(reinterpret_cast<__m128i*>(dst + at), a);
      _mm_stream_si128(reinterpret_cast<__m128i*>(dst + at + 16), b);
      _mm_stream_si128(reinterpret_cast<__m128i*>(dst + at + 32), c);
      _mm_stream_si128(reinterpret_cast<__m128i*>(dst + at + 48), d);
    }
    _mm_sfence();
  }
#endif
  std::memcpy(dst + at, src + at, size - at);
}

// Queue what was gathered since the last sync with a copy of guest RAM as it
// is now. GPU thread.
void ship() {
  if (g_batch.bytes.empty() && g_batch.events.empty())
    return;
  const auto start = Clock::now();
  int slot;
  {
    std::unique_lock lock(g_mutex);
    g_cv.wait(lock, [] { return !g_free_slots.empty(); });
    slot = g_free_slots.back();
    g_free_slots.pop_back();
  }
  const auto got_slot = Clock::now();
  // The pages the renderer has read so far, in runs of consecutive pages.
  const uint32_t pages = g_ram_size >> kPageShift;
  g_snapshots[slot].resize(g_ram_size);
  g_snapshot_pages[slot].assign(pages, 0);
  uint8_t* valid = g_snapshot_pages[slot].data();
  if (g_watch) {
    // Arm a page before copying it: a write after that faults and stamps it, and the next sync
    // copies it again.
    const uint32_t gen = g_watch->begin_sync();
    for (auto& slot_gen : g_slot_gen)
      slot_gen.resize(pages);
    g_page_written.resize(pages);
    uint32_t* copied_at = g_slot_gen[slot].data();
    uint8_t* const dst = g_snapshots[slot].data();
    const auto copy_page = [&](uint32_t page) {
      const size_t at = static_cast<size_t>(page) << kPageShift;
      copy_streaming(dst + at, g_live_ram + at, size_t(1) << kPageShift);
      copied_at[page] = gen;
      g_snapshot_bytes += size_t(1) << kPageShift;
    };
    for (uint32_t page = 0; page < pages; ++page) {
      if (!g_page_needed[page].load(std::memory_order_relaxed))
        continue;
      valid[page] = 1;
      uint8_t& written_bits = g_page_written[page];
      if (page_hot(written_bits) && (gen + page) % kProbe != 0) {
        copy_page(page);  // left unarmed: written most syncs
        continue;
      }
      // A hot page probed now was unarmed all along, so rearm reports it written; its history
      // restarts here and the next sync's rearm tells whether it is still being written.
      const bool probe = page_hot(written_bits);
      const bool unarmed = g_watch->rearm(page);
      written_bits = probe ? 0 : static_cast<uint8_t>((written_bits << 1) | (unarmed ? 1 : 0));
      if (unarmed) {
        // Written since it was last armed, or never armed: whatever the other slots hold of it
        // may predate writes nobody stamped.
        for (int other = 0; other < kSlots; ++other)
          if (other != slot)
            g_slot_gen[other][page] = 0;
        ++g_rearmed_pages;
        copy_page(page);
        continue;
      }
      const uint32_t written = g_watch->write_gen[page].load(std::memory_order_acquire);
      if (copied_at[page] == 0 || written >= copied_at[page]) {
        copy_page(page);
      } else if (g_watch_check) {
        const size_t at = static_cast<size_t>(page) << kPageShift;
        if (std::memcmp(dst + at, g_live_ram + at, size_t(1) << kPageShift) != 0)
          ++g_watch_mismatches;
      }
    }
  }
  for (uint32_t page = 0; !g_watch && page < pages;) {
    if (!g_page_needed[page].load(std::memory_order_relaxed)) {
      ++page;
      continue;
    }
    uint32_t end = page + 1;
    while (end < pages && g_page_needed[end].load(std::memory_order_relaxed))
      ++end;
    const size_t at = static_cast<size_t>(page) << kPageShift;
    copy_streaming(g_snapshots[slot].data() + at, g_live_ram + at, static_cast<size_t>(end - page) << kPageShift);
    std::memset(valid + page, 1, end - page);
    g_snapshot_bytes += static_cast<uint64_t>(end - page) << kPageShift;
    page = end;
  }
  g_batch.slot = slot;
  {
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(g_batch));
    g_batch = Batch{};
    if (!g_spare_batches.empty()) {
      g_batch = std::move(g_spare_batches.back());
      g_spare_batches.pop_back();
    }
  }
  g_sync_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(got_slot - start).count();
  g_sync_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - got_slot).count();
  ++g_syncs;
  g_cv.notify_all();
}
}  // namespace

// Starts Aurora (window AURORA_NATIVE_SIZE=<w>x<h>, default 1920x1080) over
// the given guest RAM (MEM1). Returns false if Aurora could not start.
AURORA_NATIVE_API bool aurora_native_init(const uint8_t* ram, uint32_t ram_size) {
  g_live_ram = ram;
  g_ram_size = ram_size;
  if (!g_async) {
    g_ram = ram;
    g_ready = start_aurora();
    return g_ready;
  }
  for (int i = 0; i < kSlots; ++i)
    g_free_slots.push_back(i);
  g_page_needed = std::make_unique<std::atomic<uint8_t>[]>(ram_size >> kPageShift);
  if (g_main_thread_renderer) {
    g_ready = start_aurora();
    NATIVE_LOG("aurora-native: rendering on the main thread, guest on worker, %d snapshot slots\n", kSlots);
    return g_ready;
  }
  std::promise<bool> started;
  auto result = started.get_future();
  g_thread = std::thread(render_thread, &started);
  g_ready = result.get();
  if (!g_ready)
    g_thread.join();
  NATIVE_LOG("aurora-native: rendering on its own thread, %d snapshot slots\n", kSlots);
  return g_ready;
}

// Standalone Cocoa host: opt in before initialization, then execute the guest
// callback on a worker while this calling main thread owns every video operation.
AURORA_NATIVE_API void aurora_native_use_main_thread_renderer() {
  g_main_thread_renderer = true;
  g_async = true;
}

AURORA_NATIVE_API void aurora_native_wake_main_thread() { g_cv.notify_all(); }

AURORA_NATIVE_API void aurora_native_run_main_thread(void (*guest)(void*), void* data,
                                                     void (*poll_main_thread)()) {
  if (!g_main_thread_renderer) {
    guest(data);
    return;
  }
  std::thread emulation([=] {
#ifdef __APPLE__
    pthread_setname_np("Colosseum guest");
#endif
    guest(data);
    // Queue the final partial frame before telling the main thread to drain.
    ship();
    {
      std::lock_guard lock(g_mutex);
      g_stop = true;
    }
    g_cv.notify_all();
  });
  render_loop(poll_main_thread);
  emulation.join();
  NATIVE_LOG("aurora-native: guest worker stopped; main-thread queue drained\n");
}

// The runner's write watch over guest RAM; see WriteWatch. Called on the GPU thread after init.
AURORA_NATIVE_API bool aurora_native_write_watch(const WriteWatch* watch) {
  if (!g_async || !g_ready || !watch || watch->version != 1)
    return false;
  g_watch = watch;
  g_watch_on = true;
  std::fprintf(stderr, "aurora-native: write watch on: snapshots recopy only written pages\n");
  return true;
}

// Whole GX commands, in stream order.
AURORA_NATIVE_API void aurora_native_feed(const uint8_t* data, uint32_t size) {
  if (!g_ready || size == 0)
    return;
  if (!g_async) {
    render_bytes(data, size);
    return;
  }
  g_batch.bytes.insert(g_batch.bytes.end(), data, data + size);
}

// The frame's XFB copy has executed: present it. `aspect` is the draw aspect
// Dolphin's presenter would use (menus 4:3, widescreen gameplay wider).
AURORA_NATIVE_API void aurora_native_frame_end(float aspect) {
  if (!g_ready)
    return;
  g_last_aspect = aspect;
  if (!g_async) {
    render_present(aspect, fill_kind());
    if (g_interpolation_layers)
      aurora_backend_set_frame_interpolation(g_interpolation_layers, g_interpolation_fields);
    return;
  }
  g_batch.events.push_back({.kind = Event::Present, .offset = g_batch.bytes.size(), .aspect = aspect,
                            .interpolation_layers = g_interpolation_layers.load(),
                            .interpolation_fields = g_interpolation_fields.load(),
                            .naming_fill = fill_kind()});
  // A game that presents without GXDrawDone in between (loading screens)
  // still gets its frames shown, at most one frame late.
  if (++g_batch.presents >= 2)
    ship();
}

// The GPU read [address, address + size) at this point in the stream (a TLUT
// load); keep those bytes as they are now for the render thread.
AURORA_NATIVE_API void aurora_native_capture(uint32_t address, uint32_t size) {
  if (!g_ready || !g_async)
    return;
  const uint32_t physical = address & 0x01FFFFFFu;
  if (physical >= g_ram_size || size > g_ram_size - physical || size == 0)
    return;
  const size_t data = g_batch.patch_bytes.size();
  g_batch.patch_bytes.insert(g_batch.patch_bytes.end(), g_live_ram + physical, g_live_ram + physical + size);
  g_batch.events.push_back(
      {.kind = Event::Patch, .offset = g_batch.bytes.size(), .address = physical, .size = size, .data = data});
}

// GXDrawDone: the game now waits for the GPU, so guest RAM holds what this
// batch's draws read. Called before the finish interrupt is raised.
AURORA_NATIVE_API void aurora_native_sync() {
  if (g_ready && g_async)
    ship();
}

// Colosseum text from a host font (gxruntime/aurora_backend.h): the runner's
// rasterizer, called on the render thread with each reported glyph.
AURORA_NATIVE_API void aurora_native_set_text_rasterizer(AuroraTextRasterizer rasterizer) {
  aurora_backend_set_text_rasterizer(rasterizer);
}

// Replacement textures (gxruntime/aurora_backend.h): the runner's texture-pack
// folders, loaded by the render thread before its next draw.
AURORA_NATIVE_API void aurora_native_set_texture_pack(const char* const* dirs, size_t count) {
  aurora_backend_set_texture_pack(dirs, count);
}

// Before aurora_native_init: the window's size and vsync.
AURORA_NATIVE_API void aurora_native_configure(unsigned width, unsigned height, bool vsync) {
  if (width && height) {
    g_config_width = width;
    g_config_height = height;
  }
  g_config_vsync = vsync;
}

// Whether status lines are printed (default on).
AURORA_NATIVE_API void aurora_native_set_verbose(bool verbose) { g_verbose = verbose; }

// Before aurora_native_init: a host overlay; see g_overlay_draw.
AURORA_NATIVE_API void aurora_native_set_overlay(void (*draw)(bool imgui_frame),
                                                 void (*snap)(uint8_t* rgb, uint32_t width, uint32_t height)) {
  g_overlay_draw = draw;
  g_overlay_snap = snap;
}

// Present again without new commands: the last frame stays on screen (the
// EFB keeps it) under the overlay, and the window's events keep flowing while
// the game is paused.
AURORA_NATIVE_API void aurora_native_repaint() {
  if (!g_ready)
    return;
  if (!g_async) {
    render_present(g_last_aspect, fill_kind());
    return;
  }
  g_batch.events.push_back({.kind = Event::Present, .offset = g_batch.bytes.size(), .aspect = g_last_aspect, .interpolation_layers = 1,
                            .naming_fill = fill_kind()});
  ship();
}

// Before aurora_native_init: present a frame_end aspect of 1.5 or more at
// exactly that aspect (fitted into the window, with bars where the window's
// shape differs) instead of stretched to the window. Below 1.5 frames are
// fitted at the game's 4:3 either way.
AURORA_NATIVE_API void aurora_native_set_exact_aspect(bool exact) { g_exact_aspect = exact; }

// The host asks to quit, as closing the window does.
AURORA_NATIVE_API void aurora_native_request_quit() { g_quit_requested = true; }

// The player closed Aurora's window, or the host asked to quit.
AURORA_NATIVE_API bool aurora_native_should_quit() {
  if (!g_ready)
    return false;
  return g_quit_requested.load() || (!g_async && aurora_backend_should_quit());
}

AURORA_NATIVE_API void aurora_native_shutdown() {
  if (!g_ready)
    return;
  g_ready = false;
  if (g_main_thread_renderer)
    return;  // run_main_thread already joined the guest and drained its queue
  if (!g_async)
    return;  // nothing runs on its own; see render_thread on why no teardown
  ship();
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  g_thread.join();
  NATIVE_LOG("aurora-native: stopped\n");
}

#ifdef _WIN32
// Aurora is never torn down (see render_thread), so at process exit this DLL's
// static destructors would run against a GPU device and driver already gone:
// a fatal device loss, joinable worker threads, pipelines released into a
// torn-down D3D12 driver. DllMain runs before the CRT's static teardown, so end
// the process here with its exit code once Aurora has been up.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr && g_aurora_up) {
    DWORD code = 0;
    if (!GetExitCodeProcess(GetCurrentProcess(), &code) || code == STILL_ACTIVE)
      code = 0;
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), code);
  }
  return TRUE;
}
#else
// The same on Linux, where exit() runs this library's static destructors (and
// those Aurora registered while starting) in reverse order of registration: a
// handler registered once Aurora is up runs before all of them and ends the
// process with exit()'s status.
#if defined(__GLIBC__)
void end_before_teardown(int status, void*) {
  std::fflush(nullptr);
  _exit(status);
}
#else
void end_before_teardown() {
  // The standalone exits explicitly with its status after initialization.
  // This fallback protects hosts that reach normal static teardown.
  std::fflush(nullptr);
  _exit(EXIT_SUCCESS);
}
#endif
#endif

AURORA_NATIVE_API void aurora_native_set_frame_rate(unsigned rate, bool allowed, unsigned fields) {
  fields = std::clamp(fields, 1u, 2u);
  g_interpolation_fields = fields;
  g_interpolation_layers = frame_interpolation_layers(rate, allowed, fields);
}

AURORA_NATIVE_API void aurora_native_configure_texture_pack(const char* path) {
  g_texture_pack = path ? path : "";  // before initialization
}

AURORA_NATIVE_API void aurora_native_set_pda_fill(bool enabled) { g_pda_fill_requested = enabled; }

AURORA_NATIVE_API void aurora_native_set_naming_fill(bool enabled) {
  g_naming_fill_requested = enabled;
}
