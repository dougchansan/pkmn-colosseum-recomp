// SPDX-License-Identifier: GPL-3.0-or-later
// GC6E01 addresses/layouts: pkmn-colosseum config/GC6E01/symbols.txt,
// camera.c, gs_render_util.h, gs_gfx.h and dolphin/pad/Pad.h.
#include "moderngekko/mod_abi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define TOGGLE 0x0810u       /* Z + Y */
#define FIRST_PERSON 0x0410u /* Z + X */
#define RESET 0x0014u        /* Z + D-pad Down */
#define FIRST_PERSON_EYE_HEIGHT 14.0f
#define ORBIT_GROUND_CLEARANCE 4.0f /* third-person eye above Wes's floor */
#define ZOOM_BUTTONS 0x000Cu /* D-pad Up / Down */

static struct {
  uint32_t pad, camera, floor, entry, floor_id;
  unsigned buttons, previous, scope, enabled, initialized, pending;
  unsigned sample, applied_sample;
  uint64_t poll_time;
  uint32_t poll_pc, poll_sp;
  unsigned poll_seen;
  unsigned orbit_ready, orbit_saved, heading_pending, first_person;
  unsigned battle; /* main camera is the battle camera (game mode 8) */
  unsigned area, area_valid, rebase_area, entrance_pending, restore_pending;
  float authored_yaw;
  float orbit_radius;
  float orbit_yaw, orbit_pitch;
  float orbit_zoom, zoom_axis;
  uint32_t heading_address, near_address;
  uint8_t saved_near[4];
  uint32_t head, head_scale_cache;
  unsigned head_depth;
  uint32_t hidden_body;          /* player model hidden for this batch draw */
  uint8_t saved_body_flags[2];   /* its flag bytes 1-2 (in-view bits) */
  uint8_t saved_head_scale[12], saved_head_matrix[48], saved_head_flags[4], saved_head_cache[12];
  unsigned near_pending;
  unsigned fov_pending, fov_wanted;
  uint32_t fov_address;
  uint8_t saved_fov[4];      /* CObj +0x40 perspective fov (vertical degrees) */
  unsigned far_pending;
  uint32_t far_address;
  uint8_t saved_far[4];      /* CObj +0x3C far plane */
  uint8_t saved_heading[4];
  float x, y, z, yaw, pitch;
  float move_x, move_y, look_x, look_y, vertical;
  uint8_t saved_eye[12], saved_interest[12], saved_up[12], saved_look;
} cam;

static unsigned sensitivity_percent = 100;
/* First-person turn speed (percent; 0 = same as sensitivity_percent). */
static unsigned first_person_sensitivity_percent = 0;
/* Look acceleration (0 off, 1 normal, 2 fast) and this frame's shaping factor;
   see first_person_look_shaping. */
static unsigned look_acceleration = 1;
static float look_shaping = 1.0f;
/* First-person horizontal field of view in degrees, measured at 16:9 like PC
   shooters (0 = the game's own camera). Set by moderngekko.camera.set_fov. */
static unsigned first_person_fov = 0;
/* Free-camera FOV, same units (0 = the game camera's own). Set by
   moderngekko.camera.set_free_fov; the attract-remake staging uses it. */
static unsigned free_fov = 0;

/* Photo mode (pause menu): the world's clock stops (see ticks_entry), the free
   camera flies on real time, the game sees an idle controller, and the view
   can take its own FOV. */
static struct {
  unsigned on;
  unsigned fov;  /* horizontal degrees at 16:9, 0 = the camera's own */
  unsigned saved_enabled, saved_first_person;
} photo;
static unsigned invert_y;

static uint8_t *memory(CPUState *s, uint32_t a, uint32_t size) {
  // GSgfxState lives in the Gekko locked cache, outside ordinary MEM1.
  // Let the host validate and expose that range through the existing ABI.
  if (a >= 0xE0000000u && a - 0xE0000000u < 0x4000u &&
      size <= 0x4000u - (a - 0xE0000000u) && s->external_pointer)
    return s->external_pointer(s, a, size);
  uint32_t offset = a - 0x80000000u;
  if (!s->ram || a < 0x80000000u || offset > s->ram_size ||
      size > s->ram_size - offset)
    return NULL;
  return s->ram + offset;
}

static uint32_t read32(CPUState *s, uint32_t a) {
  const uint8_t *p = memory(s, a, 4);
  return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | p[3]
           : 0;
}

// GSresGetResource(0, Wes handle), using its validated GC6E01 table layout.
static uint32_t player_model(CPUState *s) {
  uint32_t table = read32(s, 0x8047AC5Cu);
  uint32_t count = read32(s, 0x8047AC60u);
  uint32_t handle = read32(s, 0x8047D030u);
  if (!handle || count > 4096 || !memory(s, table, count * 0x14u))
    return 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t entry = table + i * 0x14u;
    if (read32(s, entry + 4) && !read32(s, entry + 8) &&
        read32(s, entry + 12) == handle)
      return read32(s, entry + 4);
  }
  return 0;
}

static float read_float(const uint8_t *p) {
  uint32_t bits = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | p[3];
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static void write_float(uint8_t *p, float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  p[0] = (uint8_t)(bits >> 24);
  p[1] = (uint8_t)(bits >> 16);
  p[2] = (uint8_t)(bits >> 8);
  p[3] = (uint8_t)bits;
}

static void write32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static float axis(uint8_t raw) {
  int v = (int)(int8_t)raw;
  int magnitude = v < 0 ? -v : v;
  if (magnitude <= 12)
    return 0;
  if (magnitude > 80)
    magnitude = 80;
  return (float)(magnitude - 12) / 68.0f * (v < 0 ? -1.0f : 1.0f);
}

static void disable(void) {
  cam.enabled = cam.initialized = cam.first_person = 0;
  cam.orbit_ready = 0;
  cam.orbit_saved = 0;
  cam.rebase_area = cam.entrance_pending = 0;
  cam.orbit_zoom = 1;
  cam.zoom_axis = 0;
  cam.move_x = cam.move_y = cam.look_x = cam.look_y = cam.vertical = 0;
}

// These exports run on the CPU thread and need no guest memory.
static void configure(CPUState *s) {
  sensitivity_percent = s->gpr[3] < 50 ? 50 : s->gpr[3] > 200 ? 200 : s->gpr[3];
  invert_y = s->gpr[4] != 0;
  const uint32_t fp = s->gpr[5];
  first_person_sensitivity_percent = fp == 0 ? 0 : fp < 25 ? 25 : fp > 300 ? 300 : fp;
  look_acceleration = s->gpr[6] > 2 ? 1 : s->gpr[6];  /* r6: 0 off, 1 normal, 2 fast */
  moderngekko_mod_return_u32(s, 1);
}

static unsigned look_sensitivity(void) {
  return cam.first_person && !cam.enabled && first_person_sensitivity_percent
             ? first_person_sensitivity_percent
             : sensitivity_percent;
}

/* First-person look shaping (Settings > Look acceleration: 0 off, 1 normal,
   2 fast), like console shooters: a response curve (deflection^1.6, so small
   moves aim finely and a full push is 1.5x faster), and turn acceleration
   (holding the stick fully over ramps the speed up over ~0.5 s). Returned as
   a factor on the linear stick speed; the 240 Hz camera stamp carries the
   same factor so the renderer's prediction matches. */

static float first_person_look_shaping(float dt) {
  static float held;  /* seconds at full deflection */
  if (!look_acceleration || !cam.first_person || cam.enabled) {
    held = 0;
    return 1.0f;
  }
  const float ax = fabsf(cam.look_x), ay = fabsf(cam.look_y);
  const float m = ax > ay ? ax : ay;
  if (m <= 0.0f) {
    held = 0;
    return 1.0f;
  }
  const float curve = 1.5f * powf(m, 1.6f) / m;
  if (m > 0.92f)
    held += dt;
  else
    held = 0;
  float ramp = (held - 0.15f) / 0.35f;
  ramp = ramp < 0 ? 0 : ramp > 1 ? 1 : ramp;
  const float boost = look_acceleration >= 2 ? 1.5f : 0.8f;
  return curve * (1.0f + ramp * boost);
}

static void recenter(CPUState *s) {
  disable();
  moderngekko_mod_return_u32(s, 1);
}

/* Developer page (pause menu): the game's leftover debug-menu functions
   (collision display, memory info window, ...), queued by
   moderngekko.debug.call (r3 = guest address, r4 = its argument) and run on
   the next frame from draw_entry, where the game's own state is consistent. */
static const ModernGekkoModHostApi *api;
static struct {
  uint32_t address, arg;
} debug_calls[8];
static unsigned debug_call_count;

static void debug_call(CPUState *s) {
  const int queued = api && api->call_guest && debug_call_count < 8;
  if (queued) {
    debug_calls[debug_call_count].address = s->gpr[3];
    debug_calls[debug_call_count].arg = s->gpr[4];
    ++debug_call_count;
  }
  moderngekko_mod_return_u32(s, queued ? 1u : 0u);
}

static void run_debug_calls(CPUState *s) {
  for (unsigned i = 0; i < debug_call_count; ++i) {
    CPUState copy = *s;
    copy.gpr[3] = debug_calls[i].arg;
    const int ok = api->call_guest(api->user_data, &copy, debug_calls[i].address);
    fprintf(stderr, "[freecam] debug call %08x(%x): %s\n", debug_calls[i].address,
            debug_calls[i].arg, ok ? "done" : "failed");
  }
  debug_call_count = 0;
}

static void on_load(const ModernGekkoModHostApi *host) { api = host; }

static void set_fov(CPUState *s) {
  const uint32_t degrees = s->gpr[3];
  first_person_fov = degrees == 0 ? 0 : degrees < 50 ? 50 : degrees > 150 ? 150 : degrees;
  moderngekko_mod_return_u32(s, 1);
}

static void set_free_fov(CPUState *s) {
  const uint32_t degrees = s->gpr[3];
  free_fov = degrees == 0 ? 0 : degrees < 10 ? 10 : degrees > 150 ? 150 : degrees;
  moderngekko_mod_return_u32(s, 1);
}

static void get_settings(CPUState *s) {
  s->gpr[3] = sensitivity_percent;
  s->gpr[4] = invert_y;
}

// Camera state handover (the pause menu's Graphics API switch relaunches the
// game): mode flags in r3, orbit yaw/pitch/zoom/radius and the free-camera
// pose as raw float bits in r4..r12.
#define CAMERA_STATE_ORBIT 1u
#define CAMERA_STATE_FIRST_PERSON 2u
#define CAMERA_STATE_FREE 4u
#define CAMERA_STATE_ACTIVE 8u /* the override currently owns the view (read-only) */

static uint32_t float_bits(float f) {
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}

static float bits_float(uint32_t b) {
  float f;
  memcpy(&f, &b, 4);
  return f;
}

static void get_state(CPUState *s) {
  s->gpr[3] = (cam.orbit_saved ? CAMERA_STATE_ORBIT : 0) |
              (cam.first_person ? CAMERA_STATE_FIRST_PERSON : 0) |
              (cam.enabled && cam.initialized ? CAMERA_STATE_FREE : 0) |
              (cam.orbit_ready ? CAMERA_STATE_ACTIVE : 0);
  s->gpr[4] = float_bits(cam.orbit_yaw);
  s->gpr[5] = float_bits(cam.orbit_pitch);
  s->gpr[6] = float_bits(cam.orbit_zoom);
  s->gpr[7] = float_bits(cam.orbit_radius);
  s->gpr[8] = float_bits(cam.x);
  s->gpr[9] = float_bits(cam.y);
  s->gpr[10] = float_bits(cam.z);
  s->gpr[11] = float_bits(cam.yaw);
  s->gpr[12] = float_bits(cam.pitch);
}

static void set_state(CPUState *s) {
  const uint32_t flags = s->gpr[3];
  if (!(flags & CAMERA_STATE_ORBIT)) {
    moderngekko_mod_return_u32(s, 0);
    return;
  }
  cam.orbit_saved = 1;
  cam.orbit_yaw = bits_float(s->gpr[4]);
  cam.orbit_pitch = bits_float(s->gpr[5]);
  cam.orbit_zoom = bits_float(s->gpr[6]);
  cam.orbit_radius = bits_float(s->gpr[7]);
  cam.first_person = (flags & CAMERA_STATE_FIRST_PERSON) != 0;
  cam.entrance_pending = cam.rebase_area = 0;
  if (flags & CAMERA_STATE_FREE) {
    cam.enabled = cam.initialized = 1;
    cam.x = bits_float(s->gpr[8]);
    cam.y = bits_float(s->gpr[9]);
    cam.z = bits_float(s->gpr[10]);
    cam.yaw = bits_float(s->gpr[11]);
    cam.pitch = bits_float(s->gpr[12]);
  }
  // The first field update sees a "new" floor (nothing recorded yet); adopt it
  // instead of suspending, which would drop the restored free camera.
  cam.restore_pending = 1;
  moderngekko_mod_return_u32(s, 1);
}

static void suspend_camera(void) {
  // Door fades and scripted cameras suspend the override without forgetting
  // the player's chosen view. Reattach around the destination's player target.
  cam.enabled = cam.initialized = cam.orbit_ready = 0;
  cam.zoom_axis = 0;
  cam.move_x = cam.move_y = cam.look_x = cam.look_y = cam.vertical = 0;
  cam.applied_sample = cam.sample;
  cam.camera = cam.scope = 0;
}

static void reset_floor(CPUState *s) {
  (void)s;
  if (cam.first_person)
    cam.entrance_pending = 1;
  suspend_camera();
}

static void skip_boot_movie(CPUState *s); /* see Skip intro below */

static void pad_entry(CPUState *s) { cam.pad = s->gpr[3]; }

static void pad_return(CPUState *s) {
  skip_boot_movie(s);
  uint8_t *p = memory(s, cam.pad, 12);
  // The chassis may deliver a return through both physical and cached-PC
  // aliases. Consume each completed PADRead once, before clearing its buffer.
  if (cam.poll_seen && cam.poll_time == s->timebase && cam.poll_pc == s->pc &&
      cam.poll_sp == s->gpr[1])
    return;
  cam.poll_seen = 1;
  cam.poll_time = s->timebase;
  cam.poll_pc = s->pc;
  cam.poll_sp = s->gpr[1];
#if defined(FREECAM_TRACE)
  static unsigned traced, last_buttons;
  unsigned buttons = p ? ((unsigned)p[0] << 8) | p[1] : 0;
  if (traced++ < 4 || (buttons != last_buttons && (buttons & TOGGLE) == TOGGLE))
    fprintf(
        stderr,
        "[freecam] pad ptr=%08x buttons=%02x%02x err=%d camera=%08x scope=%u\n",
        cam.pad, p ? p[0] : 0, p ? p[1] : 0, p ? (int)(int8_t)p[10] : -99,
        cam.camera, cam.scope);
  last_buttons = buttons;
#endif
  if (!p || p[10] != 0) {
    suspend_camera();
    cam.previous = 0;
    return;
  }
  cam.buttons = ((unsigned)p[0] << 8) | p[1];
  cam.move_x = axis(p[2]);
  cam.move_y = axis(p[3]);
  cam.look_x = axis(p[4]);
  cam.look_y = axis(p[5]);
#if defined(FREECAM_TRACE)
  {
    static unsigned traced_axes;
    if ((cam.look_x || cam.look_y || cam.move_x || cam.move_y) &&
        traced_axes++ < 8)
      fprintf(stderr,
              "[freecam] axes enabled=%u sample=%u move=%f,%f look=%f,%f\n",
              cam.enabled, cam.sample, cam.move_x, cam.move_y, cam.look_x,
              cam.look_y);
  }
#endif
  cam.vertical = ((cam.buttons & 0x20u) || p[7] > 128 ? 1.0f : 0.0f) -
                 ((cam.buttons & 0x40u) || p[6] > 128 ? 1.0f : 0.0f);
  if (photo.on) {
    // Photo mode: the free camera reads this sample; the game sees nothing.
    p[0] = p[1] = 0;
    for (unsigned i = 2; i < 10; ++i)
      p[i] = 0;
    cam.previous = cam.buttons;
    cam.sample++;
    return;
  }
  if ((cam.buttons & RESET) == RESET)
    disable();
  else if ((cam.buttons & TOGGLE) == TOGGLE &&
           (cam.previous & TOGGLE) != TOGGLE && cam.scope == 0) {
    // Eligibility is established by the last field camera update.
    if (cam.camera) {
      cam.enabled = !cam.enabled;
      cam.first_person = 0;
      cam.initialized = 0;
      fprintf(stderr, "[freecam] %s\n", cam.enabled ? "enabled" : "disabled");
    }
  } else if ((cam.buttons & FIRST_PERSON) == FIRST_PERSON &&
           (cam.previous & FIRST_PERSON) != FIRST_PERSON && cam.camera &&
           cam.scope == 0 && !cam.battle) {
    cam.first_person = !cam.first_person;
    if (cam.first_person)
      cam.orbit_pitch = 0; // Begin at eye level, rather than looking down at feet.
    cam.enabled = cam.initialized = 0;
  }
  // Read physical X/Y for camera chords, then reserve them before the
  // game's input module sees them (including camera suspension frames).
  p[0] &= (uint8_t)~0x0Cu;
  cam.previous = cam.buttons;
  cam.sample++;
  unsigned zoom_controls = cam.camera && cam.orbit_ready && !cam.enabled && !cam.battle &&
                           !cam.first_person && read32(s, 0x8040836Cu) != 1;
  cam.zoom_axis = zoom_controls && !(cam.buttons & 0x10u)
                      ? ((cam.buttons & 4u) ? 1.0f : 0.0f) -
                            ((cam.buttons & 8u) ? 1.0f : 0.0f)
                      : 0;
  // Reserve camera controls, including the toggle/release poll, before
  // the game's input module copies PADStatus. Port 1 only.
  if (cam.enabled || (cam.camera && ((cam.buttons & TOGGLE) == TOGGLE ||
                                     (cam.buttons & RESET) == RESET ||
                                     (cam.buttons & FIRST_PERSON) == FIRST_PERSON)))
    memset(p, 0, 10);
  else if (cam.camera && cam.orbit_ready && !cam.battle) {
    p[4] = p[5] = 0; // C-stick belongs to the third-person camera.
    if (zoom_controls)
      p[1] = (uint8_t)(p[1] & ~ZOOM_BUTTONS); // Zoom must not move the player.
  }
}

static void heading_entry(CPUState *s) {
  // Only player movement reads the override. Other camera users retain the
  // game's authored heading, and menu navigation keeps its original axes.
  if (!cam.orbit_ready || cam.enabled || cam.heading_pending ||
      (s->lr != 0x80186FC8u && s->lr != 0x8012E74Cu))
    return;
  uint32_t state = read32(s, 0x80478C40u);
  uint8_t *p = memory(s, state + 0x14u, 4);
  if (!p)
    return;
  cam.heading_address = state + 0x14u;
  memcpy(cam.saved_heading, p, 4);
  write_float(p, remainderf(cam.orbit_yaw + 3.14159265f, 6.2831853f));
  cam.heading_pending = 1;
}

static void heading_return(CPUState *s) {
  if (!cam.heading_pending)
    return;
  uint8_t *p = memory(s, cam.heading_address, 4);
  if (p)
    memcpy(p, cam.saved_heading, 4);
  cam.heading_pending = 0;
}

static void update_entry(CPUState *s) {
  uint32_t state = read32(s, 0x80478C40u);
  uint8_t *mode = memory(s, state, 1);
  uint8_t *field = memory(s, 0x8047AD71u, 1);
  uint32_t floor = read32(s, 0x8047ACC8u);
  uint32_t entry = read32(s, floor);
  uint32_t id = entry ? read32(s, entry + 0xCu) : 0;
  if (cam.floor != floor || cam.entry != entry || cam.floor_id != id) {
    if (cam.restore_pending) {
      cam.restore_pending = 0;  // state handed over: keep it
    } else {
      if (cam.first_person)
        cam.entrance_pending = 1;
      suspend_camera();
    }
    cam.floor = floor;
    cam.entry = entry;
    cam.floor_id = id;
  }
  // Fixed indoor cameras can have no interpolation-bank entries. The field
  // flag, mode and main-camera validation still exclude menus/cinematics.
  // Battles (mode 8) drive the same main camera through cameraUpdate: the free
  // camera works there too, while the battle's own cameras stay untouched.
  // Mode 7 is the battle's own camera sequences (intro, random owner shots).
  cam.battle = mode && (mode[0] == 7 || mode[0] == 8) && entry;
  cam.scope = (mode && mode[0] == 0 && field && *field && entry) || cam.battle;
#if defined(FREECAM_TRACE)
  {
    static unsigned traced;
    if (traced++ < 4)
      fprintf(
          stderr,
          "[freecam] update mode=%u field=%u scope=%u floor=%08x entry=%08x\n",
          mode ? *mode : 255, field ? *field : 0, cam.scope, floor, entry);
  }
#endif
  if (!cam.scope) {
    suspend_camera();
  } else if (!cam.battle) {
    // floorDataBiosGetArea: canonical floor entry byte +1, shared by the
    // rooms within a city. Ignore world-map/loading entries outside field scope.
    const uint8_t *area = memory(s, entry + 1u, 1);
    if (area) {
      if (cam.area_valid && cam.area != *area && cam.orbit_saved)
        cam.rebase_area = 1;
      cam.area = *area;
      cam.area_valid = 1;
    }
  }
}

static void update_return(CPUState *s) {
  (void)s;
  cam.scope = 0;
}

// Put the game's own FOV back before it loads the camera again, so its logic and
// any authored camera animation see their own value.
static void restore_fov(CPUState *s) {
  if (!cam.fov_pending)
    return;
  uint8_t *fov = memory(s, cam.fov_address, 4);
  if (fov)
    memcpy(fov, cam.saved_fov, 4);
  cam.fov_pending = 0;
}

/* 240 Hz camera: every game frame's camera, stamped into the GX FIFO as an XF
   load to an address outside XF memory (0x7E00). The renderer receives it in
   order with that frame's draws and rotates each frame-interpolation layer to
   where the latest look input puts the camera when the layer is shown. Words:
   0xCA02 << 16 | flags << 8 | mode, yaw, pitch, look speed (rad/s at full
   deflection), the time in nanoseconds on the renderer's steady clock (high,
   low), the eye position, and the orbit radius (third-person orbit only). */
#define CAMERA_STAMP_ADDRESS 0x7E00u
#define CAMERA_STAMP_FIRST_PERSON 1u
#define CAMERA_STAMP_FREE 2u
#define CAMERA_STAMP_ORBIT 3u

static uint64_t monotonic_ns(void) {
#ifdef _WIN32
  /* The conversion MSVC's std::chrono::steady_clock uses. */
  static LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (!frequency.QuadPart)
    QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  const long long f = frequency.QuadPart, c = counter.QuadPart;
  if (f == 10000000)
    return (uint64_t)c * 100u;
  return (uint64_t)((c / f) * 1000000000LL + (c % f) * 1000000000LL / f);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

/* p: the main camera after this frame's eye/interest were written (NULL when
   stopping). Version 2 adds the eye position and, for the third-person orbit,
   the distance to the target it circles. */
static void stamp_camera(CPUState *s, int off, const uint8_t *p) {
  if (!s->external_write)
    return;
  unsigned mode = off || !p ? 0u
                  : cam.enabled ? CAMERA_STAMP_FREE
                  : cam.first_person ? CAMERA_STAMP_FIRST_PERSON
                  : (cam.orbit_ready && !cam.battle) ? CAMERA_STAMP_ORBIT : 0u;
  float ex = 0, ey = 0, ez = 0, yaw = 0, pitch = 0, radius = 0;
  if (mode) {
    ex = read_float(p + 0x70);
    ey = read_float(p + 0x74);
    ez = read_float(p + 0x78);
    const float dx = read_float(p + 0x100) - ex, dy = read_float(p + 0x104) - ey,
                dz = read_float(p + 0x108) - ez;
    const float length = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(length) || length < 0.001f) {
      mode = 0;
    } else {
      yaw = atan2f(dx, dz);
      pitch = asinf(fmaxf(-1, fminf(1, dy / length)));
      if (mode == CAMERA_STAMP_ORBIT)
        radius = length;
    }
  }
  const uint64_t ns = monotonic_ns();
  const uint32_t words[10] = {
      0xCA02u << 16 | (invert_y ? 1u : 0u) << 8 | mode,
      float_bits(yaw),
      float_bits(pitch),
      float_bits(2.0f * (float)look_sensitivity() / 100.0f * look_shaping),
      (uint32_t)(ns >> 32),
      (uint32_t)ns,
      float_bits(ex),
      float_bits(ey),
      float_bits(ez),
      float_bits(radius),
  };
  s->external_write(s, 0xCC008000u, 0x10u, 1); /* GX_LOAD_XF_REG */
  s->external_write(s, 0xCC008000u, (9u << 16) | CAMERA_STAMP_ADDRESS, 4);
  for (unsigned i = 0; i < 10; ++i)
    s->external_write(s, 0xCC008000u, words[i], 4);
}

static void load_entry(CPUState *s) {
  restore_fov(s);
  uint32_t address = read32(s, 0x8047AA74u);
  uint8_t *p = memory(s, address, 0x128);
  uint8_t *cobj = p ? memory(s, read32(s, address + 0xCu), 0x51) : NULL;

  // Render passes reload the main camera again after cameraUpdate returns.
  // Reapply to that identified camera, while leaving other cameras alone.
  if (!cam.scope && (!cam.orbit_ready || address != cam.camera))
    return;
  // A battle free camera stays on through the battle's camera changes (its
  // random owner and move cameras switch or animate the camera object).
  const int battle_free = cam.battle && cam.enabled && cam.initialized;
  if (!p || !cobj || (p[4] && !battle_free) || cobj[0x50] != 1) {
    /* A scripted camera takes over: stop the renderer's 240 Hz rotation now. */
    if (cam.orbit_ready && (cam.enabled || cam.first_person))
      stamp_camera(s, 1, NULL);
    suspend_camera();
    return;
  }
  if (cam.camera && cam.camera != address && !battle_free)
    suspend_camera();
  cam.camera = address;
  if (cam.pending)
    return;
  if (cam.battle) {
    cam.first_person = 0;
    if (!cam.enabled) {
      // Leave the battle's own (animated) cameras alone until Z+Y.
      cam.applied_sample = cam.sample;
      return;
    }
    if (!cam.initialized) {
      // Detach from exactly the view the battle camera was showing.
      float ex = read_float(p + 0x70), ey = read_float(p + 0x74), ez = read_float(p + 0x78);
      float tx = read_float(p + 0x100) - ex, ty = read_float(p + 0x104) - ey,
            tz = read_float(p + 0x108) - ez;
      float length = sqrtf(tx * tx + ty * ty + tz * tz);
      if (!isfinite(length) || length < 0.001f) {
        cam.applied_sample = cam.sample;
        return;
      }
      cam.x = ex;
      cam.y = ey;
      cam.z = ez;
      cam.yaw = atan2f(tx, tz);
      cam.pitch = asinf(fmaxf(-1, fminf(1, ty / length)));
      cam.initialized = 1;
    }
    cam.orbit_ready = 1; // keeps this camera claimed between loads
    // The battle's camera animation sets near/far for its own shot every frame;
    // a free view looking across the arena would clip the floor at that far
    // plane. Push it out for this load (restored in load_return).
    uint8_t *far_plane = memory(s, read32(s, address + 0xCu) + 0x3Cu, 4);
    if (far_plane && !cam.far_pending) {
      const float far_value = read_float(far_plane);
      if (isfinite(far_value) && far_value < 30000.0f) {
        memcpy(cam.saved_far, far_plane, 4);
        cam.far_address = read32(s, address + 0xCu) + 0x3Cu;
        write_float(far_plane, 30000.0f);
        cam.far_pending = 1;
      }
    }
  }
#if defined(FREECAM_TRACE)
  {
    static unsigned traced_loads;
    if (traced_loads++ < 8)
      fprintf(stderr, "[freecam] load sample=%u applied=%u look=%f,%f\n",
              cam.sample, cam.applied_sample, cam.look_x, cam.look_y);
  }
#endif
  float target_x = read_float(p + 0x100);
  float target_y = read_float(p + 0x104);
  float target_z = read_float(p + 0x108);
  float dx = target_x - read_float(p + 0x70);
  float dy = target_y - read_float(p + 0x74);
  float dz = target_z - read_float(p + 0x78);
  float radius = sqrtf(dx * dx + dy * dy + dz * dz);
  if (!isfinite(radius) || radius < 0.001f || !isfinite(target_x) ||
      !isfinite(target_y) || !isfinite(target_z)) {
    suspend_camera();
    return;
  }
  float authored_yaw = atan2f(dx, dz);
  if (cam.battle)
    authored_yaw = cam.authored_yaw; // keep the field's reference heading
  if (!cam.orbit_ready) {
    if (cam.rebase_area && cam.orbit_saved) {
      // Carry the chosen offset into the destination's coordinate frame.
      // Absolute world yaw is retained for doors within the same area.
      cam.orbit_yaw = remainderf(cam.orbit_yaw + authored_yaw -
                                cam.authored_yaw, 6.2831853f);
      cam.rebase_area = 0;
    }
    if (!cam.orbit_saved) {
      cam.orbit_yaw = atan2f(dx, dz);
      cam.orbit_pitch = asinf(fmaxf(-1, fminf(1, dy / radius)));
      cam.orbit_zoom = 1;
      cam.orbit_radius = radius;
      cam.orbit_saved = 1;
    }
    cam.orbit_ready = 1;
  }
  if (cam.first_person && cam.entrance_pending) {
    uint32_t model = player_model(s);
    const uint8_t *rotation = memory(s, model + 0x28u, 4);
    float facing = rotation ? read_float(rotation) : NAN;
    if (isfinite(facing)) {
      // The arrival script spawns Wes facing out from the entrance. Adopt
      // that heading, rather than carrying the source room's look offset.
      cam.orbit_yaw = remainderf(facing, 6.2831853f);
      cam.orbit_pitch = 0;
      cam.applied_sample = cam.sample;
      cam.entrance_pending = cam.rebase_area = 0;
    }
  }
  cam.authored_yaw = authored_yaw;
  radius = cam.orbit_radius;
  if (cam.enabled && !cam.initialized && !cam.battle) {
    // Detach from the view the player was just using.
    cam.yaw = cam.orbit_yaw;
    cam.pitch = cam.orbit_pitch;
    float orbit_radius = radius * cam.orbit_zoom;
    cam.x = target_x - sinf(cam.yaw) * cosf(cam.pitch) * orbit_radius;
    cam.y = target_y - sinf(cam.pitch) * orbit_radius;
    cam.z = target_z - cosf(cam.yaw) * cosf(cam.pitch) * orbit_radius;
    cam.initialized = 1;
  }
  /* One stamp per new controller sample, i.e. per game frame. */
  const int stamp = cam.applied_sample != cam.sample;
  if (cam.applied_sample != cam.sample) {
    uint32_t gfx = read32(s, 0x8047AA80u);
    uint32_t delta = read32(s, gfx + 0x54u);
    uint32_t rate = read32(s, 0x8047AA94u);
    float dt = rate && delta ? (float)delta / (float)rate : 0;
#if defined(FREECAM_TRACE)
    {
      static unsigned traced_time;
      if ((cam.look_x || cam.move_y || cam.vertical) && traced_time++ < 8)
        fprintf(stderr,
                "[freecam] motion gfx=%08x delta=%u rate=%u dt=%f yaw=%f\n",
                gfx, delta, rate, dt, cam.yaw);
    }
#endif
    float sy, cy, cp, sp, speed, mx = cam.move_x, my = cam.move_y;
    float magnitude = sqrtf(mx * mx + my * my);
    if (dt > 0.1f)
      dt = 0.1f;
    cam.applied_sample = cam.sample;
    // C_MTXLookAt uses up cross (eye - target) as screen-right.
    float *yaw = cam.enabled ? &cam.yaw : &cam.orbit_yaw;
    float *pitch = cam.enabled ? &cam.pitch : &cam.orbit_pitch;
    look_shaping = first_person_look_shaping(dt);
    float look_speed = 2.0f * (float)look_sensitivity() / 100.0f * look_shaping * dt;
    *yaw = remainderf(*yaw - cam.look_x * look_speed, 6.2831853f);
    *pitch += cam.look_y * look_speed * (invert_y ? -1.0f : 1.0f);
    float upper_pitch = (cam.enabled || cam.first_person) ? 1.45f : 0.25f;
    if (*pitch > upper_pitch)
      *pitch = upper_pitch;
    float lower_pitch = (cam.enabled || cam.first_person) ? -1.45f : -1.35f;
    if (*pitch < lower_pitch)
      *pitch = lower_pitch;
    if (!cam.enabled)
      cam.orbit_zoom =
          fmaxf(0.35f, fminf(2.5f, cam.orbit_zoom + cam.zoom_axis * 0.8f * dt));
    sy = sinf(cam.yaw);
    cy = cosf(cam.yaw);
    cp = cosf(cam.pitch);
    sp = sinf(cam.pitch);
    if (magnitude > 1) {
      mx /= magnitude;
      my /= magnitude;
    }
    speed = 250.0f * dt;
    if (cam.enabled) {
      cam.x += (my * sy * cp - mx * cy) * speed;
      cam.y += (my * sp + cam.vertical) * speed;
      cam.z += (my * cy * cp + mx * sy) * speed;
    }
  }
  memcpy(cam.saved_eye, p + 0x70, 12);
  memcpy(cam.saved_interest, p + 0x100, 12);
  memcpy(cam.saved_up, p + 0xF4, 12);
  cam.saved_look = p[1];
  p[1] = p[2] = 1;
  if (cam.enabled) {
    if ((photo.on && photo.fov) || free_fov) {
      cam.near_address = read32(s, address + 0xCu) + 0x38u;
      cam.fov_wanted = cobj[0x50] == 1;
    }
    write_float(p + 0x70, cam.x);
    write_float(p + 0x74, cam.y);
    write_float(p + 0x78, cam.z);
    write_float(p + 0x100, cam.x + sinf(cam.yaw) * cosf(cam.pitch) * 100);
    write_float(p + 0x104, cam.y + sinf(cam.pitch) * 100);
    write_float(p + 0x108, cam.z + cosf(cam.yaw) * cosf(cam.pitch) * 100);
  } else if (cam.first_person) {
    // Validated player ground transform, also present in building interiors.
    uint32_t model = player_model(s);
    // The global mirror's Y can remain zero on elevated indoor floors.
    const uint8_t *player = model ? memory(s, model + 0x18u, 12) :
                                   memory(s, 0x80452EE4u, 12);
    float px = player ? read_float(player) : NAN;
    float py = player ? read_float(player + 4) : NAN;
    float pz = player ? read_float(player + 8) : NAN;
    if (!isfinite(px) || !isfinite(py) || !isfinite(pz)) {
      memcpy(p + 0x70, cam.saved_eye, 12);
      memcpy(p + 0x100, cam.saved_interest, 12);
      memcpy(p + 0xF4, cam.saved_up, 12);
      p[1] = cam.saved_look;
      return;
    }
    // Third-person near planes can clip away an entire room at eye level.
    cam.near_address = read32(s, address + 0xCu) + 0x38u;
    memcpy(cam.saved_near, cobj + 0x38, 4);
    write_float(cobj + 0x38, 1.0f);
    cam.near_pending = 1;
    // The projection is rebuilt from the CObj when the frame is drawn, after this
    // load returns, so the FOV is applied in load_return and kept until the next
    // camera load (see restore_fov).
    cam.fov_wanted = first_person_fov && cobj[0x50] == 1;
    float fx = sinf(cam.orbit_yaw) * cosf(cam.orbit_pitch);
    float fy = sinf(cam.orbit_pitch);
    float fz = cosf(cam.orbit_yaw) * cosf(cam.orbit_pitch);
    // Place the eye ahead of the head to reduce clipping through Wes's model.
    px += sinf(cam.orbit_yaw) * 2;
    py += FIRST_PERSON_EYE_HEIGHT;
    pz += cosf(cam.orbit_yaw) * 2;
    write_float(p + 0x70, px);
    write_float(p + 0x74, py);
    write_float(p + 0x78, pz);
    write_float(p + 0x100, px + fx * 100);
    write_float(p + 0x104, py + fy * 100);
    write_float(p + 0x108, pz + fz * 100);
  } else {
    radius *= cam.orbit_zoom;
    write_float(p + 0x70, target_x - sinf(cam.orbit_yaw) *
                                         cosf(cam.orbit_pitch) * radius);
    float eye_y = target_y - sinf(cam.orbit_pitch) * radius;
    // Keep the eye above the floor Wes stands on, so looking up (or orbiting
    // over stairs and slopes) never puts the camera under the ground.
    uint32_t model = player_model(s);
    const uint8_t *player = model ? memory(s, model + 0x18u, 12) : NULL;
    const float ground_y = player ? read_float(player + 4) : NAN;
    if (isfinite(ground_y) && eye_y < ground_y + ORBIT_GROUND_CLEARANCE)
      eye_y = ground_y + ORBIT_GROUND_CLEARANCE;
    write_float(p + 0x74, eye_y);
    write_float(p + 0x78, target_z - cosf(cam.orbit_yaw) *
                                         cosf(cam.orbit_pitch) * radius);
  }
  write_float(p + 0xF4, 0);
  write_float(p + 0xF8, 1);
  write_float(p + 0xFC, 0);
  if (stamp)
    stamp_camera(s, 0, p);
  cam.pending = 1;
}

static void load_return(CPUState *s) {
  uint8_t *p;
  if (cam.far_pending) {
    uint8_t *far_plane = memory(s, cam.far_address, 4);
    if (far_plane)
      memcpy(far_plane, cam.saved_far, 4);
    cam.far_pending = 0;
  }
  if (!cam.pending)
    return;
  p = memory(s, cam.camera, 0x128);
  if (p) {
    memcpy(p + 0x70, cam.saved_eye, 12);
    memcpy(p + 0x100, cam.saved_interest, 12);
    memcpy(p + 0xF4, cam.saved_up, 12);
    p[1] = cam.saved_look;
    p[2] = 1; // Rebuild the authored matrix on the next normal load.
  }
  if (cam.near_pending) {
    uint8_t *near_plane = memory(s, cam.near_address, 4);
    if (near_plane)
      memcpy(near_plane, cam.saved_near, 4);
    cam.near_pending = 0;
  }
  if (cam.fov_wanted) {
    // Horizontal FOV at 16:9 -> the vertical FOV the CObj stores; wider displays
    // then widen it further (Hor+) through the aspect hook. CObj fov sits 8 bytes
    // after its near plane (+0x38 -> +0x40).
    uint8_t *fov = memory(s, cam.near_address + 8u, 4);
    if (fov) {
      const unsigned degrees = photo.on && photo.fov ? photo.fov
                               : cam.enabled && free_fov ? free_fov
                                                         : first_person_fov;
      const float half = (float)degrees * 0.5f * 3.14159265f / 180.0f;
      memcpy(cam.saved_fov, fov, 4);
      cam.fov_address = cam.near_address + 8u;
      write_float(fov, 2.0f * atanf(tanf(half) / (16.0f / 9.0f)) * 180.0f / 3.14159265f);
      cam.fov_pending = 1;
    }
    cam.fov_wanted = 0;
  }
  cam.pending = 0;
}

// Head geometry shares display objects with the coat/body. Collapse only
// its skinning joint during Wes's draw, preserving the remaining skeleton.
static void head_entry(CPUState *s) {
  if (cam.head_depth) {
    cam.head_depth++;
    return;
  }
  if (!cam.first_person || !cam.orbit_ready || cam.enabled)
    return;
  uint32_t model = player_model(s);
  if (!memory(s, model, 0x30u))
    return;
  uint32_t root = read32(s, model + ((read32(s, model) & 0x80u) ? 12u : 8u));
  if (!root || s->gpr[3] != model)
    return;
  uint32_t mesh = read32(s, root + 0x10u);
  uint32_t dobj = read32(s, mesh + 0x18u);
  uint32_t pobj = read32(s, dobj + 12u);
  uint32_t list = read32(s, pobj + 20u);
  uint32_t envelope = read32(s, list + 4u);
  uint32_t head = read32(s, envelope + 4u);
  uint8_t *joint = memory(s, head, 0x78u);
  // The first mesh is wholly weighted to a single leaf head joint in Wes's
  // field model. Fail closed for a different model/topology.
  if (!joint || read32(s, list) || read32(s, envelope) ||
      read32(s, head + 0x10u) ||
      read32(s, envelope + 8u) != 0x3F800000u)
    return;
  float height = read_float(joint + 0x60u) -
                 read_float(memory(s, model + 0x1Cu, 4));
  if (!isfinite(height) || height < 8 || height > 25)
    return;
  cam.head_scale_cache = read32(s, head + 0x74u);
  uint8_t *cache = memory(s, cam.head_scale_cache, 12);
  if (cache)
    memcpy(cam.saved_head_cache, cache, 12);
  memcpy(cam.saved_head_scale, joint + 0x2Cu, 12);
  memcpy(cam.saved_head_matrix, joint + 0x44u, 48);
  memcpy(cam.saved_head_flags, joint + 0x14u, 4);
  for (unsigned i = 0; i < 3; i++)
    write_float(joint + 0x2Cu + i * 4u, 0.001f);
  joint[0x17] |= 0x40u; // Recompute the head matrix from the temporary scale.
  cam.head = head;
  cam.head_depth = 1;
}

static void head_return(CPUState *s) {
  if (!cam.head_depth || --cam.head_depth)
    return;
  uint8_t *joint = memory(s, cam.head, 0x78u);
  if (joint) {
    memcpy(joint + 0x2Cu, cam.saved_head_scale, 12);
    memcpy(joint + 0x44u, cam.saved_head_matrix, 48);
    memcpy(joint + 0x14u, cam.saved_head_flags, 4);
    joint[0x17] |= 0x40u;
  }
  uint8_t *cache = memory(s, cam.head_scale_cache, 12);
  if (cache)
    memcpy(cache, cam.saved_head_cache, 12);
  cam.head = cam.head_scale_cache = 0;
}

// GSmodel table (gs_model_main): 0x170-byte records at *0x8047AB74, count at
// *0x8047AB78. fn_800E3928 (per-frame model update) tests each model with
// bound checking (0x00100000) against the game's authored camera via
// fn_80191118 and sets 0x00400000 only when it passes; fn_800E3604 draws a
// model only if (flags & 0x00400400). The orbit/free camera looks from
// elsewhere, so that test drops background models the player can see - and
// as the authored camera follows the player they flip in and out (black
// flicker stepping off the Pyrite stairs). Mark active, visible models as in
// view right before the batch draw while the override owns the view.
#define MODEL_TABLE 0x8047AB74u
#define MODEL_COUNT 0x8047AB78u
#define MODEL_STRIDE 0x170u
#define MODEL_ACTIVE_VISIBLE 0x00000003u
#define MODEL_BOUND_CHECK 0x00100000u
#define MODEL_IN_VIEW 0x00400000u

static void force_models_in_view(CPUState *s) {
  if (!cam.orbit_ready)
    return;
  uint32_t table = read32(s, MODEL_TABLE);
  uint32_t count = read32(s, MODEL_COUNT);
  if (!table || count > 4096u)
    return;
  uint8_t *records = memory(s, table, count * MODEL_STRIDE);
  if (!records)
    return;
  unsigned forced = 0, checked = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint8_t *p = records + i * MODEL_STRIDE;
    uint32_t flags = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3];
    if ((flags & MODEL_ACTIVE_VISIBLE) != MODEL_ACTIVE_VISIBLE || !(flags & MODEL_BOUND_CHECK))
      continue;
    checked++;
    if (!(flags & MODEL_IN_VIEW)) {
      p[1] |= (uint8_t)(MODEL_IN_VIEW >> 16);
      forced++;
    }
  }
  static int trace = -1;
  if (trace < 0) {
    const char *v = getenv("FREECAM_CULL_TRACE");
    trace = v && v[0] == '1';
  }
  if (trace) {
    static unsigned calls, forced_total, forced_max, checked_last;
    calls++;
    forced_total += forced;
    if (forced > forced_max)
      forced_max = forced;
    checked_last = checked;
    if (calls % 120 == 0) {
      fprintf(stderr, "[freecam] cull: %u draws, bound-checked=%u forced avg=%.1f max=%u\n",
              calls, checked_last, forced_total / 120.0, forced_max);
      forced_total = forced_max = 0;
    }
  }
}

// Run (left stick click in the runtime): Wes and Rui move and animate twice
// as fast. heroMoveMain moves both each frame (src/game/hero_move.c):
// - Wes: fn_80188214 (people.c) steps pos += speed * fn_800D3088(), the
//   retraces since the last frame, then resolves collision with a swept test
//   - doubling that count for this call doubles the move and keeps collision.
// - Rui follows Wes's trail, her step capped at fn_800D3088() units per frame
//   (fn_8012D7F0, call returning to 0x8012D834): doubling it lets her keep up.
// - updateAnimation (0x8012C660) sets the gait's rate (0.5, or 1.0 for the
//   backing-off branch) at model+0x94, not every frame: write twice that
//   absolute rate after it, and the base rate back when running stops.
static struct {
  unsigned on;
  unsigned in_player_step;  // inside fn_80188214 called by the player controller
  unsigned double_ticks;    // frame delta doubled for this fn_800D3088 call
  uint32_t ticks_address;
  uint32_t saved_ticks;
  uint32_t anim_model;
  unsigned anim_member;
  float anim_amount;
  uint32_t scaled_model[2];  // models whose rate this mod set (Wes, Rui)
} run;

/* Game speed (pause menu): text and battle pacing, skipping the boot intro.
   Research: C:/dev/colo-perf/research/speed/REPORT.md (decomp addresses). */
#define FIGHT_ACTIVE 0x8047B5DAu /* u8: a battle is in progress */
static struct {
  unsigned text;         /* 0 normal, 1 fast (2x), 2 faster (4x), 3 instant */
  unsigned battle;       /* battle pacing multiplier, 1..4 */
  unsigned skip_intro;
  unsigned text_depth;   /* inside the dialogue renderer fn_800FC7E0 */
  uint32_t instant_task; /* message task whose mode byte was set for instant text */
  uint8_t saved_mode;
  uint32_t anim_model;   /* model whose animation rates are scaled for this advance */
  uint8_t saved_rates[8];
  unsigned intro_redirect; /* boot floor selector: redirect this call */
  uint8_t saved_logo_dwell[4];
  unsigned logo_pending;
  unsigned intro_done;
  unsigned enemy_level_percent; /* difficulty: trainer Pokemon levels +N% */
  uint32_t level_record;        /* source record whose level was raised */
  uint8_t saved_level;
} speed = {0, 1, 0};

// r3 = text speed (0..3), r4 = battle multiplier (1..4), r5 = skip intro,
// r6 = enemy level increase in percent (0..100).
static void configure_game(CPUState *s) {
  speed.text = s->gpr[3] > 3 ? 3 : s->gpr[3];
  speed.battle = s->gpr[4] < 1 ? 1 : s->gpr[4] > 4 ? 4 : s->gpr[4];
  speed.skip_intro = s->gpr[5] != 0;
  speed.enemy_level_percent = s->gpr[6] > 100 ? 100 : s->gpr[6];
  moderngekko_mod_return_u32(s, 1);
}

static int in_battle(CPUState *s) {
  const uint8_t *flag = memory(s, FIGHT_ACTIVE, 1);
  return flag && flag[0] != 0;
}

// Dialogue renderer scope (its frame delta drives the typewriter).
static void text_entry(CPUState *s) {
  (void)s;
  speed.text_depth++;
}
static void text_return(CPUState *s) {
  (void)s;
  if (speed.text_depth)
    speed.text_depth--;
}

// Instant text: fn_800FBE7C(message id) renders a message task with its mode
// byte (+0x44); any nonzero mode skips the per-character pause. Tasks live at
// *(*(0x80478B08) + 0x20), count u16 at +0, stride 0x68, id at +0x1C.
static void message_entry(CPUState *s) {
  speed.instant_task = 0;
  if (speed.text != 3)
    return;
  const uint32_t manager = read32(s, 0x80478B08u);
  const uint8_t *header = manager ? memory(s, manager, 0x24) : NULL;
  if (!header)
    return;
  const unsigned count = ((unsigned)header[0] << 8) | header[1];
  const uint32_t tasks = read32(s, manager + 0x20u);
  if (!tasks || count > 64 || !memory(s, tasks, count * 0x68u))
    return;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t *task = memory(s, tasks + i * 0x68u, 0x68);
    if (task[0] == 1 && read32(s, tasks + i * 0x68u + 0x1Cu) == s->gpr[3]) {
      speed.instant_task = tasks + i * 0x68u;
      speed.saved_mode = task[0x44];
      if (task[0x44] == 0)
        task[0x44] = 1;
      return;
    }
  }
}
static void message_return(CPUState *s) {
  if (!speed.instant_task)
    return;
  uint8_t *task = memory(s, speed.instant_task, 0x68);
  if (task)
    task[0x44] = speed.saved_mode;
  speed.instant_task = 0;
}

// Battle: animation rates (GSmodelAdvanceAnimation, model +0x94 skeletal and
// +0xAC texture), scaled for this advance only.
static void anim_advance_entry(CPUState *s) {
  speed.anim_model = 0;
  if (speed.battle <= 1 || !in_battle(s))
    return;
  uint8_t *model = memory(s, s->gpr[3], 0xB0);
  if (!model)
    return;
  speed.anim_model = s->gpr[3];
  memcpy(speed.saved_rates, model + 0x94, 4);
  memcpy(speed.saved_rates + 4, model + 0xAC, 4);
  write_float(model + 0x94, read_float(model + 0x94) * (float)speed.battle);
  write_float(model + 0xAC, read_float(model + 0xAC) * (float)speed.battle);
}
static void anim_advance_return(CPUState *s) {
  if (!speed.anim_model)
    return;
  uint8_t *model = memory(s, speed.anim_model, 0xB0);
  if (model) {
    memcpy(model + 0x94, speed.saved_rates, 4);
    memcpy(model + 0xAC, speed.saved_rates + 4, 4);
  }
  speed.anim_model = 0;
}

// Battle HP / EXP bars (fn_800129A8, r3 = window): the window freework at
// +0x9C counts elapsed (+4 HP, +0xE EXP) up to duration (+2, +0xC) one per
// call; add the extra steps (real progress, so no restore).
static void gauge_entry(CPUState *s) {
  if (speed.battle <= 1 || !in_battle(s))
    return;
  uint8_t *work = memory(s, s->gpr[3] + 0x9Cu, 0x10);
  if (!work)
    return;
  static const unsigned kFields[2][2] = {{2, 4}, {0xC, 0xE}};
  for (unsigned i = 0; i < 2; ++i) {
    const int duration = (int16_t)((work[kFields[i][0]] << 8) | work[kFields[i][0] + 1]);
    int elapsed = (int16_t)((work[kFields[i][1]] << 8) | work[kFields[i][1] + 1]);
    if (duration <= 0 || elapsed >= duration)
      continue;
    elapsed += (int)speed.battle - 1;
    if (elapsed > duration)
      elapsed = duration;
    work[kFields[i][1]] = (uint8_t)(elapsed >> 8);
    work[kFields[i][1] + 1] = (uint8_t)elapsed;
  }
}

// Skip intro: the boot movies (floor 0x399 The Pokemon Company, 0x39A Genius
// Sonority) play through the THP player at 0x8046AC60. The per-frame movie
// update (fn_801E16F0) stops, closes and frees a movie whose player state
// (+0xA4, internal state +0xA5) is 3 (played) or 5; marking a playing movie
// as played ends it through the game's own cleanup, and each screen then moves
// on as when its movie finishes. Only these boot floors, so the new-game
// opening movie (0x3A0) and story movies still play.
#define THP_PLAYER 0x8046AC60u
static uint32_t current_floor(CPUState *s) {
  const uint32_t context = read32(s, 0x8047ACC8u);
  const uint32_t entry = context ? read32(s, context) : 0;
  return entry ? read32(s, entry + 0xCu) : 0;
}

static void skip_boot_movie(CPUState *s) {
  if (getenv("FREECAM_BOOT_TRACE")) {
    static uint32_t last_floor = 0xFFFFFFFFu;
    static uint8_t last_state = 0xFF;
    const uint32_t f = current_floor(s);
    const uint8_t *pl = memory(s, THP_PLAYER, 0xA8);
    const uint8_t st = pl ? pl[0xA4] : 0xEE;
    if (f != last_floor || st != last_state)
      fprintf(stderr, "[boot] t=%.2f floor=0x%X movie_state=%u\n",
              (double)(monotonic_ns() / 1000000ull) / 1000.0, (unsigned)f, st);
    last_floor = f;
    last_state = st;
  }
  if (!speed.skip_intro || speed.intro_done)
    return;
  const uint32_t floor = current_floor(s);
  if (floor == 0x384u) {  // the title screen: the intro is over
    speed.intro_done = 1;
    return;
  }
  if (floor != 0x399u && floor != 0x39Au)
    return;
  uint8_t *player = memory(s, THP_PLAYER, 0xA8);
  if (!player || read32(s, THP_PLAYER + 0xA0u) == 0 || player[0xA4] != 2)
    return;  // not open or not playing yet
  player[0xA4] = 3;
  player[0xA5] = 3;
  fprintf(stderr, "[freecam] skip intro: ended boot movie on floor 0x%X\n", (unsigned)floor);
}

// Skip intro: the startup logo screen waits until a timer reaches the float at
// 0x8047BA34 (2.0 s); make it 0 for this call.
static void logo_entry(CPUState *s) {
  if (getenv("FREECAM_BOOT_TRACE"))
    fprintf(stderr, "[boot] t=%.2f logo screen start\n", (double)(monotonic_ns() / 1000000ull) / 1000.0);
  speed.logo_pending = 0;
  if (!speed.skip_intro || speed.intro_done)
    return;
  uint8_t *limit = memory(s, 0x8047BA34u, 4);
  if (!limit)
    return;
  memcpy(speed.saved_logo_dwell, limit, 4);
  write_float(limit, 0.0f);
  speed.logo_pending = 1;
}
static void logo_return(CPUState *s) {
  if (getenv("FREECAM_BOOT_TRACE"))
    fprintf(stderr, "[boot] t=%.2f logo screen end\n", (double)(monotonic_ns() / 1000000ull) / 1000.0);
  if (!speed.logo_pending)
    return;
  uint8_t *limit = memory(s, 0x8047BA34u, 4);
  if (limit)
    memcpy(limit, speed.saved_logo_dwell, 4);
  speed.logo_pending = 0;
}

// Skip intro: the first floor the game selects at boot (0x399, the TPC movie,
// then 0x39A Genius Sonority, then 0x384 the title) is redirected to the
// title. fn_800FF788 selects a registered floor; after it, point the floor
// context (*(0x8047ACC8): +0 entry, +4 id + 0x5001) and the destination
// (0x8040837C) at floor 0x384. Once per boot.
static void floor_select_entry(CPUState *s) {
  // fn_800FF788(floorId) only selects while the loader state (0x8047ACD8) is 0.
  // Disabled: redirecting the first boot floor straight to the title hangs
  // (the title floor needs what the movie floors set up). Kept for reference.
  speed.intro_redirect = 0;
  (void)s;
}
static void floor_select_return(CPUState *s) {
  if (!speed.intro_redirect)
    return;
  speed.intro_redirect = 0;
  speed.intro_done = 1;
  const uint32_t table = read32(s, 0x8047ACD0u);
  const uint32_t count = read32(s, 0x8047ACD4u);
  const uint32_t context = read32(s, 0x8047ACC8u);
  uint8_t *ctx = context ? memory(s, context, 0x14) : NULL;
  if (!table || !ctx || count > 2048 || !memory(s, table, count * 0x4Cu))
    return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * 0x4Cu;
    if (read32(s, entry + 0xCu) != 0x384u)
      continue;
    write32(ctx, entry);
    write32(ctx + 4, 0x384u + 0x5001u);
    uint8_t *destination = memory(s, 0x8040837Cu, 4);
    if (destination)
      write32(destination, 0x384u);
    fprintf(stderr, "[freecam] skip intro: boot floor 0x399 -> title 0x384\n");
    return;
  }
}

/* Time of day (pause menu): the field's lights and fog are tinted while the
   frame draws and restored afterwards, so the game's own values never change.
   GS light pool *(0x8047AAEC), count *(0x8047AAF0), stride 0x74: +0 allocated,
   +1 active, +0xC HSD_LObj* (+8 u16 flags, type = flags & 3, 0 = ambient;
   +0x10 RGBA). Fog: HSD_FogSet(fog) uploads fog +0x18 RGBA.
   Research: C:/dev/colo-perf/research/lighting/REPORT.md. */
#define TINT_MAX_LIGHTS 64
static struct {
  unsigned on;
  float ambient[3], light[3], fog[3];
  unsigned depth;  /* nested draw transactions (0x800D3FA4 and model draws) */
  unsigned saved_count;
  uint32_t saved_address[TINT_MAX_LIGHTS];
  uint8_t saved_rgb[TINT_MAX_LIGHTS][3];
  uint32_t fog_address;
  uint8_t fog_rgb[3];
} tint;

// r3..r11 = ambient RGB, light RGB, fog RGB gains x1000 (0 for all = off).
static void set_tint(CPUState *s) {
  unsigned any = 0;
  for (int i = 0; i < 3; ++i) {
    tint.ambient[i] = (float)s->gpr[3 + i] / 1000.0f;
    tint.light[i] = (float)s->gpr[6 + i] / 1000.0f;
    tint.fog[i] = (float)s->gpr[9 + i] / 1000.0f;
    any |= s->gpr[3 + i] | s->gpr[6 + i] | s->gpr[9 + i];
  }
  tint.on = any != 0;
  moderngekko_mod_return_u32(s, 1);
}

static uint8_t tint_channel(uint8_t value, float gain) {
  const float v = (float)value * gain + 0.5f;
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static int tint_wanted(CPUState *s) {
  return tint.on && !photo.on && !in_battle(s);
}

static void tint_begin(CPUState *s) {
  if (tint.depth++ != 0 || !tint_wanted(s))
    return;
  tint.saved_count = 0;
  const uint32_t pool = read32(s, 0x8047AAECu);
  const uint32_t count = read32(s, 0x8047AAF0u);
  if (!pool || count > 256 || !memory(s, pool, count * 0x74u))
    return;
  for (uint32_t i = 0; i < count && tint.saved_count < TINT_MAX_LIGHTS; ++i) {
    const uint8_t *entry = memory(s, pool + i * 0x74u, 0x10);
    if (!entry[0] || !entry[1])
      continue;
    const uint32_t lobj = read32(s, pool + i * 0x74u + 0xCu);
    uint8_t *light = lobj ? memory(s, lobj, 0x14) : NULL;
    if (!light)
      continue;
    const unsigned type = light[9] & 3;  /* BE u16 flags at +8 */
    const float *gain = type == 0 ? tint.ambient : tint.light;
    tint.saved_address[tint.saved_count] = lobj;
    memcpy(tint.saved_rgb[tint.saved_count], light + 0x10, 3);
    for (int c = 0; c < 3; ++c)
      light[0x10 + c] = tint_channel(light[0x10 + c], gain[c]);
    tint.saved_count++;
  }
}

static void tint_end(CPUState *s) {
  if (!tint.depth || --tint.depth != 0)
    return;
  for (unsigned i = 0; i < tint.saved_count; ++i) {
    uint8_t *light = memory(s, tint.saved_address[i], 0x14);
    if (light)
      memcpy(light + 0x10, tint.saved_rgb[i], 3);
  }
  tint.saved_count = 0;
}

/* World marker for the renderer's time-of-day grade: an XF load to 0x7E10
   (outside XF memory), in FIFO order with the draws. 1 while the frame's 3D
   world draws, 0 around 2D sprites (spriteSetEnv) and after the frame. */
#define WORLD_MARKER_ADDRESS 0x7E10u
static struct {
  unsigned in_draw, sent, sprite_depth;
} marker;

static void send_marker(CPUState *s, unsigned value) {
  if (!s->external_write || marker.sent == value)
    return;
  s->external_write(s, 0xCC008000u, 0x10u, 1); /* GX_LOAD_XF_REG */
  s->external_write(s, 0xCC008000u, WORLD_MARKER_ADDRESS, 4);
  s->external_write(s, 0xCC008000u, value, 4);
  marker.sent = value;
}

/* Object IDs for the renderer's post effects: an XF load to 0x7E20 before
   each model draw (modelShadowPrepare(model, 1) .. (model, 0) brackets it in
   the game's model loop). 64 + n = character (a model of the people list),
   128 + slot = other model (items, Poke Balls, props), 0 = untagged/world. */
#define OBJECT_MARKER_ADDRESS 0x7E20u
static unsigned object_sent;

static void send_object_id(CPUState *s, unsigned id) {
  if (!s->external_write || object_sent == id)
    return;
  s->external_write(s, 0xCC008000u, 0x10u, 1); /* GX_LOAD_XF_REG */
  s->external_write(s, 0xCC008000u, OBJECT_MARKER_ADDRESS, 4);
  s->external_write(s, 0xCC008000u, id, 4);
  object_sent = id;
}

static unsigned classify_model(CPUState *s, uint32_t model) {
  const uint32_t people = read32(s, 0x8047B200u), people_count = read32(s, 0x8047B1F8u);
  for (uint32_t i = 0; people && i < people_count && i < 512; ++i) {
    const uint8_t *entry = memory(s, people + i * 0xDCu, 0x0C);
    if (entry && entry[0] && read32(s, people + i * 0xDCu + 0x08u) == model)
      return 64u + (i & 63u);
  }
  const uint32_t models = read32(s, 0x8047AB74u);
  return models && model >= models ? 128u + (((model - models) / 0x148u) & 63u) : 128u;
}

static void shadow_prepare_entry(CPUState *s) {
  send_object_id(s, s->gpr[4] & 0xFF ? classify_model(s, s->gpr[3]) : 0u);
}

static void draw_entry(CPUState *s) {
  if (debug_call_count)
    run_debug_calls(s);
  tint_begin(s);
  marker.in_draw = 1;
  if (tint_wanted(s))
    send_marker(s, 1);
}
static void draw_return(CPUState *s) {
  marker.in_draw = 0;
  send_marker(s, 0);
  tint_end(s);
}

static void sprite_env_entry(CPUState *s) {
  if (marker.sprite_depth++ == 0)
    send_marker(s, 0);
}
static void sprite_env_return(CPUState *s) {
  if (marker.sprite_depth && --marker.sprite_depth == 0 && marker.in_draw && tint_wanted(s))
    send_marker(s, 1);
}

static void fog_entry(CPUState *s) {
  tint.fog_address = 0;
  if (!tint_wanted(s))
    return;
  uint8_t *fog = memory(s, s->gpr[3], 0x1C);
  if (!fog)
    return;
  tint.fog_address = s->gpr[3];
  memcpy(tint.fog_rgb, fog + 0x18, 3);
  for (int c = 0; c < 3; ++c)
    fog[0x18 + c] = tint_channel(fog[0x18 + c], tint.fog[c]);
}
static void fog_return(CPUState *s) {
  if (!tint.fog_address)
    return;
  uint8_t *fog = memory(s, tint.fog_address, 0x1C);
  if (fog)
    memcpy(fog + 0x18, tint.fog_rgb, 3);
  tint.fog_address = 0;
}

// Difficulty: fightTrainerCreateFightTrainerPokemonDataIdToPokemon (0x801F9F78,
// r3 = source index) builds a trainer's Pokemon from the trainer Pokemon table
// *(0x80478F14), stride 0x50, count **(0x80478F10), level u8 at +4, before its
// stats are derived. Raise the source level for this call only (restored on
// return, so the table always holds the original).
static void trainer_mon_entry(CPUState *s) {
  speed.level_record = 0;
  if (!speed.enemy_level_percent)
    return;
  const uint32_t index = s->gpr[3] & 0xFFFFu;
  const uint32_t table = read32(s, 0x80478F14u);
  const uint32_t count_pointer = read32(s, 0x80478F10u);
  const uint32_t count = count_pointer ? read32(s, count_pointer) : 0;
  if (!table || index >= count || count > 0x4000)
    return;
  uint8_t *record = memory(s, table + index * 0x50u, 0x50);
  if (!record || record[4] == 0)
    return;
  const unsigned level = record[4];
  unsigned raised = level + (level * speed.enemy_level_percent + 50) / 100;
  if (raised > 100)
    raised = 100;
  speed.level_record = table + index * 0x50u;
  speed.saved_level = record[4];
  record[4] = (uint8_t)raised;
}
static void trainer_mon_return(CPUState *s) {
  if (!speed.level_record)
    return;
  uint8_t *record = memory(s, speed.level_record, 0x50);
  if (record)
    record[4] = speed.saved_level;
  speed.level_record = 0;
}

// r3 = on, r4 = horizontal FOV at 16:9 in degrees (0 = the camera's own).
static void set_photo(CPUState *s) {
  const unsigned on = s->gpr[3] != 0;
  const uint32_t fov = s->gpr[4];
  photo.fov = fov == 0 ? 0 : fov < 20 ? 20 : fov > 150 ? 150 : fov;
  if (on && !photo.on) {
    photo.saved_enabled = cam.enabled;
    photo.saved_first_person = cam.first_person;
    if (!cam.enabled) {
      // Detach a free camera from the current view (see load_entry).
      cam.enabled = 1;
      cam.initialized = 0;
    }
    cam.first_person = 0;
  } else if (!on && photo.on) {
    cam.enabled = photo.saved_enabled;
    cam.first_person = photo.saved_first_person;
    cam.initialized = 0;
  }
  photo.on = on;
  fprintf(stderr, "[freecam] photo mode %s (fov %u)\n", on ? "on" : "off", photo.fov);
  moderngekko_mod_return_u32(s, 1);
}

static void set_run(CPUState *s) {
  run.on = s->gpr[3] != 0;
  fprintf(stderr, "[freecam] run %s\n", run.on ? "on" : "off");
  moderngekko_mod_return_u32(s, 1);
}

static void step_entry(CPUState *s) {
  // fn_80188214(0, handle, stick) steps one person; the player is party handle
  // *(0x8047D030) (Wes), whichever controller code calls it.
  run.in_player_step = s->gpr[4] != 0 && s->gpr[4] == read32(s, 0x8047D030u);
}
static void step_return(CPUState *s) {
  (void)s;
  run.in_player_step = 0;
}

// fn_800D3088 returns the retraces since the last frame, *(*(0x8047AA80) + 0x54).
// Hooks cannot change registers (the loader restores them), so double that
// field in memory for these two calls and put it back when they return.
static void ticks_entry(CPUState *s) {
  run.double_ticks = 0;
  // One factor per call: photo mode stops the world's clock for every caller;
  // running doubles it for Wes's and Rui's steps; text speed for the dialogue
  // renderer; battle speed for the move scheduler (LR 0x801DB9A4), the move
  // camera (0x801D2D8C) and the battle waits (0x801EFFEC, 0x801F0038).
  const unsigned freeze = photo.on;
  unsigned factor = 1;
  if (run.on && ((run.in_player_step && s->lr == 0x8018854Cu) || s->lr == 0x8012D834u))
    factor = 2;
  else if (speed.text_depth && (speed.text == 1 || speed.text == 2))
    factor = speed.text == 1 ? 2 : 4;
  else if (speed.battle > 1 &&
           (s->lr == 0x801DB9A4u || s->lr == 0x801D2D8Cu || s->lr == 0x801EFFECu ||
            s->lr == 0x801F0038u) &&
           in_battle(s))
    factor = speed.battle;
  if (!freeze && factor == 1)
    return;
  const uint32_t gfx = read32(s, 0x8047AA80u);
  uint8_t *p = gfx ? memory(s, gfx + 0x54u, 4) : NULL;
  if (!p)
    return;
  const uint32_t delta = read32(s, gfx + 0x54u);
  if (!freeze && (delta == 0 || delta > 8))
    return;
  const uint32_t value = freeze ? 0 : delta * factor;
  run.ticks_address = gfx + 0x54u;
  run.saved_ticks = delta;
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
  run.double_ticks = 1;
}
static void ticks_return(CPUState *s) {
  if (!run.double_ticks)
    return;
  run.double_ticks = 0;
  uint8_t *p = memory(s, run.ticks_address, 4);
  if (p) {
    p[0] = (uint8_t)(run.saved_ticks >> 24);
    p[1] = (uint8_t)(run.saved_ticks >> 16);
    p[2] = (uint8_t)(run.saved_ticks >> 8);
    p[3] = (uint8_t)run.saved_ticks;
  }
}

// r3 = model, r4 = party member (0 Wes, 1 Rui), f1 = gait amount.
static void anim_entry(CPUState *s) {
  run.anim_model = s->gpr[4] <= 1 ? s->gpr[3] : 0;
  run.anim_member = s->gpr[4] & 1;
  run.anim_amount = (float)s->fpr[1];
}

static void anim_return(CPUState *s) {
  const uint32_t model = run.anim_model;
  run.anim_model = 0;
  if (!model)
    return;
  uint8_t *p = memory(s, model, 0x98);
  if (!p || !(p[3] & 4))
    return;
  const float base = run.anim_amount < -0.4f ? 1.0f : 0.5f;
  const unsigned member = run.anim_member;
  if (run.on) {
    write_float(p + 0x94, base * 2.0f);
    run.scaled_model[member] = model;
  } else if (run.scaled_model[member] == model) {
    write_float(p + 0x94, base);
    run.scaled_model[member] = 0;
  }
}

// First person: skip drawing Wes's body (torso, arms, coat) in the batch
// renderer by clearing its in-view bits (0x00400000 | 0x00000400, the mask the
// batch loop requires) for this draw only; restored in head_all_return.
static void hide_body(CPUState *s) {
  cam.hidden_body = 0;
  if (!cam.first_person || !cam.orbit_ready || cam.enabled)
    return;
  uint32_t model = player_model(s);
  uint8_t *p = memory(s, model, 4);
  if (!p)
    return;
  cam.saved_body_flags[0] = p[1];
  cam.saved_body_flags[1] = p[2];
  p[1] &= (uint8_t)~0x40u;
  p[2] &= (uint8_t)~0x04u;
  cam.hidden_body = model;
}

static void head_all_return(CPUState *s) {
  if (cam.hidden_body) {
    uint8_t *p = memory(s, cam.hidden_body, 4);
    if (p) {
      p[1] = cam.saved_body_flags[0];
      p[2] = cam.saved_body_flags[1];
    }
    cam.hidden_body = 0;
  }
  head_return(s);
  tint_end(s);
}

static void model_draw_entry(CPUState *s) {
  tint_begin(s);
  head_entry(s);
}
static void model_draw_return(CPUState *s) {
  head_return(s);
  tint_end(s);
}

static void head_all_entry(CPUState *s) {
  tint_begin(s);  // standalone model draws set up lights too
  force_models_in_view(s);
  hide_body(s);
  // Field models also draw through the batch renderer, which bypasses
  // GSmodelDrawModel. Cover the entire batch without changing guest arguments.
  uint32_t argument = s->gpr[3];
  s->gpr[3] = player_model(s);
  head_entry(s);
  s->gpr[3] = argument;
}

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(0x800E3604u, head_all_entry),
    RECOMP_HOOK_RETURN(0x800E3604u, head_all_return),
    RECOMP_HOOK(0x800E9148u, shadow_prepare_entry),
    RECOMP_HOOK(0x800E3760u, model_draw_entry),
    RECOMP_HOOK_RETURN(0x800E3760u, model_draw_return),
    RECOMP_HOOK(0x800D3FA4u, draw_entry),
    RECOMP_HOOK_RETURN(0x800D3FA4u, draw_return),
    RECOMP_HOOK(0x801F9F78u, trainer_mon_entry),
    RECOMP_HOOK_RETURN(0x801F9F78u, trainer_mon_return),
    RECOMP_HOOK(0x800FE4D4u, sprite_env_entry),
    RECOMP_HOOK_RETURN(0x800FE4D4u, sprite_env_return),
    RECOMP_HOOK(0x8019BD18u, fog_entry),
    RECOMP_HOOK_RETURN(0x8019BD18u, fog_return),
    RECOMP_HOOK(0x800AB150u, pad_entry),
    RECOMP_HOOK_RETURN(0x800AB150u, pad_return),
    RECOMP_HOOK(0x80177A64u, update_entry),
    RECOMP_HOOK_RETURN(0x80177A64u, update_return),
    RECOMP_HOOK(0x800D2248u, load_entry),
    RECOMP_HOOK_RETURN(0x800D2248u, load_return),
    RECOMP_HOOK(0x801794F0u, reset_floor),
    RECOMP_HOOK(0x80188214u, step_entry),
    RECOMP_HOOK_RETURN(0x80188214u, step_return),
    RECOMP_HOOK(0x800D3088u, ticks_entry),
    RECOMP_HOOK_RETURN(0x800D3088u, ticks_return),
    RECOMP_HOOK(0x8012C660u, anim_entry),
    RECOMP_HOOK_RETURN(0x8012C660u, anim_return),
    RECOMP_HOOK(0x800FC7E0u, text_entry),
    RECOMP_HOOK_RETURN(0x800FC7E0u, text_return),
    RECOMP_HOOK(0x800FBE7Cu, message_entry),
    RECOMP_HOOK_RETURN(0x800FBE7Cu, message_return),
    RECOMP_HOOK(0x800ED0D0u, anim_advance_entry),
    RECOMP_HOOK_RETURN(0x800ED0D0u, anim_advance_return),
    RECOMP_HOOK(0x800129A8u, gauge_entry),
    RECOMP_HOOK(0x8003686Cu, logo_entry),
    RECOMP_HOOK_RETURN(0x8003686Cu, logo_return),
    RECOMP_HOOK(0x800FF788u, floor_select_entry),
    RECOMP_HOOK_RETURN(0x800FF788u, floor_select_return),
    RECOMP_HOOK(0x80176684u, heading_entry),
    RECOMP_HOOK_RETURN(0x80176684u, heading_return),
};

static const ModernGekkoModExportEntry exports[] = {
    RECOMP_EXPORT("moderngekko.camera.configure", configure),
    RECOMP_EXPORT("moderngekko.camera.recenter", recenter),
    RECOMP_EXPORT("moderngekko.camera.get_settings", get_settings),
    RECOMP_EXPORT("moderngekko.camera.get_state", get_state),
    RECOMP_EXPORT("moderngekko.camera.set_state", set_state),
    RECOMP_EXPORT("moderngekko.camera.set_fov", set_fov),
    RECOMP_EXPORT("moderngekko.camera.set_free_fov", set_free_fov),
    RECOMP_EXPORT("moderngekko.player.set_run", set_run),
    RECOMP_EXPORT("moderngekko.camera.set_photo", set_photo),
    RECOMP_EXPORT("moderngekko.game.configure", configure_game),
    RECOMP_EXPORT("moderngekko.world.set_tint", set_tint),
    RECOMP_EXPORT("moderngekko.debug.call", debug_call),
};

static void unload(void) {
  memset(&cam, 0, sizeof(cam));
  sensitivity_percent = 100;
  invert_y = 0;
}

static const ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GC6E01",
    MOD_ID,
    MOD_VERSION,
    MOD_DISPLAY_NAME,
    NULL,
    0,
    NULL,
    0,
    hooks,
    sizeof(hooks) / sizeof(hooks[0]),
    exports,
    sizeof(exports) / sizeof(exports[0]),
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    on_load,
    unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc *moderngekko_get_mod(void) {
  return &descriptor;
}
