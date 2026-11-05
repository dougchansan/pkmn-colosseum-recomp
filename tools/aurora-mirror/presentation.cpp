// colosseum-standalone's widescreen presentation (see presentation.h).
//
// A port of the presentation half of ModernGekko's per-field hook
// (lib/ModernGekko/src/runtime/dolphin_runtime.cpp) and the scene predicates
// it uses (lib/ModernGekko/tools/automation_protocol.hpp); the comments there
// carry the measurements behind each guest word and hold time. Where
// ModernGekko sets Dolphin presenter modes, this decides the aspect handed to
// aurora_native_frame_end: ForceStandard is 4:3, Stretch the Hor+ target.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "presentation.h"

extern "C" void aurora_native_set_naming_fill(bool enabled);
extern "C" void aurora_native_set_pda_fill(bool enabled);

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr float kAuthoredAspect = 4.0f / 3.0f;
constexpr uint32_t kProjectionMultiplier = 0x8047E724;  // read by the guest hook at 0x80005300

// --- Guest memory -------------------------------------------------------------------------

uint8_t* g_ram = nullptr;
uint32_t g_ram_size = 0;

bool is_guest_pointer(uint32_t address) { return address >= 0x80000000u && address < 0x81800000u; }

const uint8_t* at(uint32_t address, uint32_t size) {
  const uint32_t physical = address & 0x01FFFFFFu;
  if (!g_ram || physical >= g_ram_size || size > g_ram_size - physical)
    return nullptr;
  return g_ram + physical;
}
uint8_t rd8(uint32_t a) {
  const uint8_t* p = at(a, 1);
  return p ? p[0] : 0;
}
uint16_t rd16(uint32_t a) {
  const uint8_t* p = at(a, 2);
  return p ? uint16_t(p[0] << 8 | p[1]) : 0;
}
uint32_t rd32(uint32_t a) {
  const uint8_t* p = at(a, 4);
  return p ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3] : 0;
}
float rdf(uint32_t a) { return std::bit_cast<float>(rd32(a)); }
void wr32(uint32_t a, uint32_t v) {
  const uint32_t physical = a & 0x01FFFFFFu;
  if (!g_ram || physical + 4 > g_ram_size)
    return;
  uint8_t* p = g_ram + physical;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint32_t be32(uint32_t offset) {
  return offset + 4 <= g_ram_size ? uint32_t(g_ram[offset]) << 24 | uint32_t(g_ram[offset + 1]) << 16 |
                                        uint32_t(g_ram[offset + 2]) << 8 | g_ram[offset + 3]
                                  : 0;
}

// --- Guest words (GC6E01) --------------------------------------------------------------------

constexpr uint32_t kRoomId = 0x8040837F;
constexpr uint32_t kMenuActive = 0x8040836C;
constexpr uint32_t kFieldOverlayMode = 0x80402504;
constexpr uint32_t kActiveUiText = 0x80402440;
constexpr uint32_t kActiveUiMessageId = 0x80402436;
constexpr uint32_t kStartMenuMessage = 0x013A;  // the field Start menu's root page
constexpr uint32_t kMessageWindowState = 0x80402418;
constexpr uint32_t kMovieReady = 0x8047B440;
constexpr uint32_t kMovieOpen = 0x8047B441;
constexpr uint32_t kPlayerPositionGlobal = 0x80452EE4;
constexpr uint32_t kPeopleArray = 0x8047B200;
constexpr uint32_t kPeopleCount = 0x8047B1F8;
constexpr uint32_t kPeopleStride = 0xDC;
constexpr uint32_t kPlayerIndex = 0x64;
constexpr uint32_t kColSysState = 0x80404C68;  // GSColSysState: the active collision mesh
constexpr uint32_t kFrontEndRoom = 0xA1;

// --- Scene predicates (automation_protocol.hpp) -----------------------------------------------

bool is_naming_state(uint32_t message_id, bool menu_active, unsigned room) {
  // The frontend trace identifies 0x2EED as the post-entry "Your name?"
  // prompt. Keep it scoped to its live frontend menu; 0x0069 without a menu
  // remains the title's new-Story confirmation, not character naming.
  return message_id == 0x2ef5u || message_id == 0x2efdu ||
         ((message_id == 0x0069u || message_id == 0x2eedu) && menu_active && room == kFrontEndRoom);
}
bool is_battle_now_menu(uint32_t message_id) { return message_id >= 0x3c00u && message_id <= 0x3dffu; }
bool is_battle_gameplay_message(uint32_t message_id) {
  return message_id == 0x00cfu || message_id == 0x00d0u || message_id == 0x00e7u || message_id == 0x013eu ||
         message_id == 0x0140u;
}
bool is_battle_now_arena(std::string_view archive) {
  return archive == "M1_water_colo" || archive == "M2_earth_colo" || archive == "M4_bottom_colo" ||
         archive == "M4_cylinder_colo" || archive == "T1_ancient_colo";
}
bool is_battle_field_archive(std::string_view archive) {
  return (archive.size() > 3 && archive.substr(archive.size() - 3) == "_bf") || is_battle_now_arena(archive);
}
bool retain_battle_now_arena(unsigned arena_room, unsigned current_room, bool navigation_valid) {
  return arena_room != 0 && (!navigation_valid || current_room == 0 || arena_room == current_room);
}
bool resolve_battle_session(bool session_active, bool battle_now_menu_active, bool battle_message,
                            bool navigation_valid, bool naming, bool movie, bool battle_field_archive) {
  if (battle_now_menu_active || naming || movie)
    return false;
  if (navigation_valid && !battle_field_archive)
    return false;
  return session_active || battle_message || battle_field_archive;
}
bool use_authored_menu(bool complete_menu_active, bool navigation_valid, bool naming, bool gameplay_started,
                       bool battle_gameplay_active) {
  // Naming can leave the navigation mirror valid and reuse overlay mode
  // 0x18. Neither observation turns its complete canvas into a field overlay.
  return naming || (!battle_gameplay_active && complete_menu_active && (navigation_valid || gameplay_started));
}

constexpr int kNamingHoldFields = 24;
constexpr int kFrontEndDismissFields = 240;
constexpr int kBattleGameplayConfirmFields = 3;
constexpr int kBattleNowHoldFields = 6;
constexpr int kPresentationStableFields = 3;
constexpr int kPresentationForceApplyFields = 90;

// --- Field archive names (ModernGekko's collision-map refresh) -----------------------------------

std::string archive_name_at(uint32_t offset, bool allow_dot) {
  std::string name;
  for (uint32_t i = offset; i < g_ram_size && name.size() < 63; ++i) {
    const char c = char(g_ram[i]);
    if (c == '\0')
      break;
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || (allow_dot && c == '.')))
      return {};
    name.push_back(c);
  }
  return name;
}

// The FSYS archive holding the collision mesh at `base` (FindCollisionArchiveName).
std::string collision_archive_name(uint32_t base) {
  constexpr uint32_t kFsys = 0x46535953;
  const int64_t collision = int64_t(base & 0x01FFFFFFu);
  if (collision >= int64_t(g_ram_size))
    return {};
  const int64_t first = std::max<int64_t>(0, collision - 0x800000);
  for (int64_t offset = collision & ~int64_t(3); offset >= first; offset -= 4) {
    const uint32_t o = uint32_t(offset);
    if (be32(o) != kFsys || o + 0x24 > g_ram_size)
      continue;
    const uint32_t size = be32(o + 0x20);
    const uint32_t table = be32(o + 0x18);
    if (size < 0x40 || size > 0x4000000 || uint64_t(o) + size <= uint64_t(collision) ||
        uint64_t(o) + table + 8 > g_ram_size)
      continue;
    const uint64_t name = uint64_t(o) + be32(o + table + 4);
    if (name >= g_ram_size)
      continue;
    std::string text = archive_name_at(uint32_t(name), false);
    if (!text.empty())
      return text;
  }
  return {};
}

// The first field archive resident in RAM (FindLoadedFieldArchiveName).
std::string loaded_field_archive_name() {
  constexpr uint32_t kFsys = 0x46535953;
  for (uint32_t offset = 0; offset + 0x74 <= g_ram_size; offset += 4) {
    if (be32(offset) != kFsys)
      continue;
    const uint32_t size = be32(offset + 0x20);
    if (size < 0x80 || uint64_t(offset) + size > g_ram_size)
      continue;
    std::string name = archive_name_at(offset + 0x70, true);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".fsys") == 0)
      name.resize(name.size() - 5);
    if (name.size() >= 4 && (name[0] == 'S' || name[0] == 'M' || name[0] == 'D' || name[0] == 'T') &&
        std::isdigit(static_cast<unsigned char>(name[1])) && name[2] == '_')
      return name;
  }
  return {};
}

std::string current_archive_name() {
  const uint32_t mesh = rd32(kColSysState);
  std::string name = is_guest_pointer(mesh) ? collision_archive_name(mesh) : std::string();
  return name.empty() ? loaded_field_archive_name() : name;
}

// --- State ------------------------------------------------------------------------------------

struct State {
  bool interpolation_allowed = false;
  bool field_overlay_session = false;
  bool menu_was_active = false;
  bool stale_field_overlay = false;
  bool start_menu_session = false;  // this menu opened on the Start menu's root page
  int naming_hold = 0;
  int front_end_dismiss_hold = 0;
  int battle_message_fields = 0;
  int battle_now_hold = 0;
  bool battle_session = false;
  bool gameplay_started = false;
  unsigned battle_now_arena_room = 0;
  // The loaded field archive, refreshed like ModernGekko's collision map.
  std::string archive;
  unsigned archive_room = ~0u;
  uint32_t archive_mesh = 0;
  int archive_retry_delay = 0, archive_retries = 0;
  int battle_archive_delay = 0;
  // Debounced presentation.
  bool pending_menu = false, pending_wide = false, pending_movie = false, pending_front_end = false;
  int stable_fields = 0, fields_since_apply = 0;
  bool menu_active = false, wide_active = false, movie_active = false, front_end_active = false;
  bool dirty = true;
  // What is applied.
  float present_aspect = kAuthoredAspect;
  uint32_t projection_bits = std::bit_cast<uint32_t>(1.0f);
  float front_end_hor_plus = 0.0f;
  float sent_front_end_hor_plus = -1.0f;
  bool was_widescreen = false;
  // Pyrite Town's sky (UpdateColosseumPyriteSkyCoverage).
  uint32_t sky_root = 0, sky_original_y = 0;
  bool sky_offset = false;
} g;

const bool g_trace = [] {
  const char* v = std::getenv("STANDALONE_PRESENTATION_TRACE");
  return v && *v && std::strcmp(v, "0") != 0;
}();

const bool g_trace_fields = [] {
  const char* v = std::getenv("STANDALONE_PRESENTATION_TRACE");
  return v && std::strcmp(v, "2") == 0;
}();

// fn_80191118's culling bounds follow the multiplier (WidenColosseumBoundTest).
void widen_bound_test(uint32_t projection_bits) {
  const float multiplier = std::bit_cast<float>(projection_bits);
  if (!(multiplier >= 1.0f && multiplier < 16.0f))
    return;
  const uint32_t low = std::bit_cast<uint32_t>(320.0f - 320.0f * multiplier);
  const uint32_t high = std::bit_cast<uint32_t>(320.0f + 320.0f * multiplier);
  if (rd32(0x8047D8B8) != low)
    wr32(0x8047D8B8, low);
  if (rd32(0x8047D8BC) != high)
    wr32(0x8047D8BC, high);
}

// Pyrite Town's sky dome stops short of a Hor+ frame's top corners; lower it.
void update_pyrite_sky(bool should_offset) {
  constexpr uint32_t kSkyRoot = 0x8050FDB8;
  constexpr uint32_t kTranslateY = 0x3c;
  constexpr uint32_t kLowered = 0xc3960000;  // -300.0f
  const uint32_t root = rd32(kSkyRoot);
  const auto restore = [&] {
    if (g.sky_offset && root == g.sky_root && is_guest_pointer(root) && rd32(root + kTranslateY) == kLowered)
      wr32(root + kTranslateY, g.sky_original_y);
    g.sky_root = g.sky_original_y = 0;
    g.sky_offset = false;
  };
  if (!should_offset || !is_guest_pointer(root)) {
    restore();
    return;
  }
  if (g.sky_offset && g.sky_root != root)
    restore();
  if (!g.sky_offset) {
    g.sky_root = root;
    g.sky_original_y = rd32(root + kTranslateY);
    g.sky_offset = true;
  }
  if (rd32(root + kTranslateY) != kLowered)
    wr32(root + kTranslateY, kLowered);
}

bool player_valid(unsigned* room) {
  bool valid = false;
  const uint32_t people = rd32(kPeopleArray);
  const uint32_t count = rd32(kPeopleCount);
  for (uint32_t slot = 0; is_guest_pointer(people) && slot < count && slot < 512 && !valid; ++slot) {
    const uint32_t person = people + slot * kPeopleStride;
    if (rd8(person) == 0)
      continue;
    const uint32_t model = rd32(person + 0x08);
    if (!is_guest_pointer(model))
      continue;
    const float x = rdf(model + 0x18), z = rdf(model + 0x20);
    if (!(std::isfinite(x) && std::isfinite(z) && std::abs(x) < 100000.0f && std::abs(z) < 100000.0f))
      continue;
    if (rd32(person + 0x28) == 0 && rd32(person + 0x2C) == kPlayerIndex && std::isfinite(rdf(model + 0x28)))
      valid = true;
  }
  if (!valid) {
    // ModernGekko's fallback: the player's position mirror.
    const float x = rdf(kPlayerPositionGlobal), z = rdf(kPlayerPositionGlobal + 8);
    valid = std::isfinite(x) && std::isfinite(z) && std::abs(x) < 100000.0f && std::abs(z) < 100000.0f;
  }
  *room = rd8(kRoomId);
  return valid;
}

void refresh_archive(bool valid, unsigned room) {
  const uint32_t mesh = rd32(kColSysState);
  bool refresh = valid && (g.archive_room != room || mesh != g.archive_mesh);
  if (valid)
    g.archive_mesh = mesh;
  if (!refresh && valid && g.archive_retries > 0) {
    if (g.archive_retry_delay > 0) {
      --g.archive_retry_delay;
    } else {
      refresh = true;
      g.archive_retry_delay = 30;
      --g.archive_retries;
    }
  }
  if (!refresh)
    return;
  const bool new_room = g.archive_room != room;
  g.archive_room = room;
  g.archive = current_archive_name();
  if (g.archive.empty() && new_room) {
    g.archive_retry_delay = 30;
    g.archive_retries = 6;
  } else if (!g.archive.empty()) {
    g.archive_retries = 0;
  }
  if (g_trace)
    std::fprintf(stderr, "[presentation] room 0x%02X archive '%s'\n", room, g.archive.c_str());
}

}  // namespace

void presentation_field(uint8_t* ram, uint32_t ram_size, AspectSetting setting, float window_aspect) {
  g_ram = ram;
  g_ram_size = ram_size;
  const bool widescreen = setting != AspectSetting::Standard;
  const float target_aspect =
      setting == AspectSetting::Wide ? 16.0f / 9.0f
                                     : std::clamp(std::isfinite(window_aspect) ? window_aspect : kAuthoredAspect,
                                                  kAuthoredAspect, 4.0f);

  const bool menu_active = rd32(kMenuActive) == 1;
  const uint32_t field_overlay_mode = rd8(kFieldOverlayMode);
  // Leaving the PDA leaves overlay mode 0x18 behind. The Start menu opening
  // over that stale value is a complete menu, not a field overlay; only a
  // fresh 0x18 inside it (or a menu that opens over one) is.
  if (menu_active && !g.menu_was_active) {
    g.start_menu_session = rd16(kActiveUiMessageId) == kStartMenuMessage;
    g.stale_field_overlay = field_overlay_mode == 0x18 && g.start_menu_session;
  }
  if (field_overlay_mode != 0x18)
    g.stale_field_overlay = false;
  g.menu_was_active = menu_active;
  if (!menu_active)
    g.field_overlay_session = false;
  else if (field_overlay_mode == 0x18 && !g.stale_field_overlay)
    g.field_overlay_session = true;
  // A savestate-free stand-in for the shop's nested inventory panel: its label.
  {
    constexpr std::array<uint16_t, 13> kQuantityHeld = {'Q', 'u', 'a', 'n', 't', 'i', 't',
                                                        'y', ' ', 'h', 'e', 'l', 'd'};
    const uint32_t text = rd32(kActiveUiText);
    bool shop_panel = menu_active && is_guest_pointer(text);
    for (size_t i = 0; shop_panel && i < kQuantityHeld.size(); ++i)
      shop_panel = rd16(text + uint32_t(i * 2)) == kQuantityHeld[i];
    if (shop_panel)
      g.field_overlay_session = true;
  }
  const uint32_t message_id = rd16(kActiveUiMessageId);
  const bool movie = rd8(kMovieReady) != 0 && rd8(kMovieOpen) != 0;
  const bool naming_observed = is_naming_state(message_id, menu_active, rd8(kRoomId));
  if (naming_observed)
    g.naming_hold = kNamingHoldFields;
  else if (g.naming_hold > 0)
    --g.naming_hold;
  const bool naming = naming_observed || g.naming_hold > 0;
  const bool window_live = rd8(kMessageWindowState) == 2;
  g.front_end_dismiss_hold = window_live ? kFrontEndDismissFields : std::max(g.front_end_dismiss_hold - 1, 0);
  const bool front_end_window_resident = window_live || g.front_end_dismiss_hold > 0;
  const bool battle_now_observed = is_battle_now_menu(message_id) && front_end_window_resident;
  if (is_battle_gameplay_message(message_id))
    ++g.battle_message_fields;
  else
    g.battle_message_fields = 0;
  const bool battle_message = g.battle_message_fields >= kBattleGameplayConfirmFields;
  if (battle_message && g.archive.empty() && --g.battle_archive_delay <= 0) {
    g.battle_archive_delay = 30;
    std::string archive = loaded_field_archive_name();
    if (is_battle_field_archive(archive))
      g.archive = std::move(archive);
  }
  if (battle_now_observed)
    g.battle_now_hold = kBattleNowHoldFields;
  else if (g.battle_now_hold > 0)
    --g.battle_now_hold;
  if (battle_message)
    g.battle_now_hold = 0;
  const bool battle_now_active = battle_now_observed || g.battle_now_hold > 0;
  const bool pda = menu_active && (message_id & 0xff00) == 0x3600;
  if (pda)
    g.field_overlay_session = false;
  const bool field_overlay_active = menu_active && g.field_overlay_session;
  const bool complete_menu = menu_active && !field_overlay_active;

  unsigned room = 0;
  const bool valid = player_valid(&room);
  if (g_trace && room == kFrontEndRoom) {
    static uint64_t front_end_fields = 0;
    if ((front_end_fields++ % 60) == 0) {
      const uint32_t text_pointer = rd32(kActiveUiText);
      std::array<char, 97> text{};
      if (is_guest_pointer(text_pointer)) {
        for (size_t i = 0; i < text.size() - 1; ++i) {
          const uint16_t code = rd16(text_pointer + uint32_t(i * 2));
          if (!code) break;
          text[i] = code >= 32 && code <= 126 ? char(code) : '.';
        }
      }
      std::fprintf(stderr,
                   "[presentation-frontend] sample=%llu room=0x%02X msg=0x%04X menu=%u overlay=0x%02X "
                   "window=%u text=0x%08X utf16=\"%s\" valid=%d people=0x%08X count=%u "
                   "movie=%u/%u naming=%d hold=%d\n",
                   static_cast<unsigned long long>(front_end_fields - 1), room, message_id, rd32(kMenuActive),
                   field_overlay_mode, rd8(kMessageWindowState), text_pointer, text.data(), valid,
                   rd32(kPeopleArray), rd32(kPeopleCount), rd8(kMovieReady), rd8(kMovieOpen), naming,
                   g.naming_hold);
    }
  }
  refresh_archive(valid, room);
  bool battle_field_archive = is_battle_field_archive(g.archive);
  if (is_battle_now_arena(g.archive) && g.archive_room == room)
    g.battle_now_arena_room = room;
  if (battle_now_observed || naming || movie || !retain_battle_now_arena(g.battle_now_arena_room, room, valid))
    g.battle_now_arena_room = 0;
  battle_field_archive = battle_field_archive || retain_battle_now_arena(g.battle_now_arena_room, room, valid);
  g.battle_session = resolve_battle_session(g.battle_session,
                                            battle_now_active && (!battle_field_archive || battle_now_observed),
                                            battle_message, valid, naming, movie, battle_field_archive);
  // The Start menu's Pokemon, Items and other field pages draw over the live
  // field, so they keep the Hor+ world behind them like the field overlays.
  // Only the PDA is an opaque 4:3 canvas; its sides get the PDA backdrop.
  // The PC (its Box and Summary) opens on its own pages and is an opaque 4:3
  // canvas too; only pages reached from the Start menu draw over the field.
  const bool field_page = complete_menu && g.start_menu_session && !pda && valid && g.gameplay_started && !battle_field_archive &&
                          !naming && !movie && !g.battle_session && room != kFrontEndRoom;
  const bool authored_menu =
      use_authored_menu(complete_menu && !field_page, valid, naming, g.gameplay_started,
                        // In a field with a non-battle archive the battle ids
                        // (0x00E7) belong to the Start menu's Pokemon list.
                        g.battle_session || (battle_message && !(valid && !battle_field_archive)));
  if (g_trace_fields) {
    // STANDALONE_PRESENTATION_TRACE=2: every change of the raw menu inputs.
    static uint64_t field = 0, last_key = ~0ull;
    ++field;
    const uint64_t key = uint64_t(menu_active) | uint64_t(field_overlay_mode) << 1 | uint64_t(message_id) << 9 |
                         uint64_t(authored_menu) << 25 | uint64_t(valid) << 26 | uint64_t(g.field_overlay_session) << 27 |
                         uint64_t(room) << 28;
    if (key != last_key) {
      last_key = key;
      std::fprintf(stderr, "[presentation-field] %llu room 0x%02X menu %d overlay 0x%02X session %d msg 0x%04X valid %d authored %d\n",
                   static_cast<unsigned long long>(field), room, menu_active, field_overlay_mode, g.field_overlay_session,
                   message_id, valid, authored_menu);
    }
  }
  g.interpolation_allowed = !movie && !naming && !authored_menu && !battle_now_active && room != 0 && room != kFrontEndRoom && (valid || g.battle_session);
  const bool live_title = !movie && !valid && !(naming || battle_now_active || g.battle_session);

  if (widescreen != g.was_widescreen) {
    g.was_widescreen = widescreen;
    g.dirty = true;
  }
  if (widescreen && (valid || menu_active) && !g.gameplay_started) {
    g.gameplay_started = true;
    g.dirty = true;
  }
  if (!widescreen) {
    // ModernGekko without native ultrawide: the authored projection, 4:3.
    g.present_aspect = kAuthoredAspect;
    g.projection_bits = std::bit_cast<uint32_t>(1.0f);
    g.front_end_hor_plus = 0.0f;
    g.wide_active = false;
  } else if (g.gameplay_started || authored_menu || live_title || movie) {
    const bool wide = !authored_menu && !movie && (g.gameplay_started || live_title);
    const bool front_end = room == kFrontEndRoom || battle_now_active;
    const bool invalidated = std::exchange(g.dirty, false);
    // Act on an observation once it has held for a few fields (or, if it
    // never does, after a while anyway).
    if (authored_menu != g.pending_menu || wide != g.pending_wide || movie != g.pending_movie ||
        front_end != g.pending_front_end) {
      g.pending_menu = authored_menu;
      g.pending_wide = wide;
      g.pending_movie = movie;
      g.pending_front_end = front_end;
      g.stable_fields = 1;
    } else if (g.stable_fields < kPresentationStableFields) {
      ++g.stable_fields;
    }
    ++g.fields_since_apply;
    const bool settled =
        g.stable_fields >= kPresentationStableFields || g.fields_since_apply >= kPresentationForceApplyFields;
    if (invalidated || (settled && (authored_menu != g.menu_active || wide != g.wide_active ||
                                    movie != g.movie_active || front_end != g.front_end_active))) {
      g.fields_since_apply = 0;
      g.menu_active = authored_menu;
      g.wide_active = wide;
      g.movie_active = movie;
      g.front_end_active = front_end;
      if (g_trace)
        std::fprintf(stderr,
                     "[presentation] %s: room 0x%02X msg 0x%04X menu %d/%d valid %d naming %d movie %d title %d "
                     "battle_now %d battle %d/%d archive '%s'\n",
                     wide ? "Hor+" : "4:3", room, message_id, menu_active, complete_menu, valid, naming, movie,
                     live_title, battle_now_active, battle_message, g.battle_session, g.archive.c_str());
    }
    // ModernGekko presents ForceWide for the remaining case (wide off with
    // neither a menu nor a movie); with the authored projection that would
    // stretch 4:3 content, so it is pillarboxed here.
    g.present_aspect = g.wide_active ? target_aspect : kAuthoredAspect;
    const float multiplier = g.wide_active ? target_aspect / kAuthoredAspect : 1.0f;
    g.projection_bits = std::bit_cast<uint32_t>(multiplier);
    g.front_end_hor_plus = g.front_end_active && g.wide_active ? multiplier : 0.0f;
  }
  aurora_native_set_naming_fill(widescreen && naming && !movie && !g.wide_active);
  aurora_native_set_pda_fill(widescreen && pda && g.menu_active && !movie && !g.wide_active);
  presentation_enforce(ram, ram_size);
  update_pyrite_sky(widescreen && g.gameplay_started && !authored_menu && !movie && room == 0x0f &&
                    g.archive == "M2_out");
}

void presentation_enforce(uint8_t* ram, uint32_t ram_size) {
  g_ram = ram;
  g_ram_size = ram_size;
  if (rd32(kProjectionMultiplier) != g.projection_bits)
    wr32(kProjectionMultiplier, g.projection_bits);
  widen_bound_test(g.projection_bits);
}

bool presentation_interpolation_allowed() { return g.interpolation_allowed; }

float presentation_present_aspect() { return g.present_aspect; }

bool presentation_take_bp(uint8_t out[5], bool resync) {
  if (!resync && g.front_end_hor_plus == g.sent_front_end_hor_plus)
    return false;
  g.sent_front_end_hor_plus = g.front_end_hor_plus;
  const uint32_t fixed = uint32_t(std::clamp(g.front_end_hor_plus, 0.0f, 4095.0f) * 4096.0f);
  out[0] = 0x61;
  out[1] = 0x6F;
  out[2] = uint8_t(fixed >> 16);
  out[3] = uint8_t(fixed >> 8);
  out[4] = uint8_t(fixed);
  return true;
}
