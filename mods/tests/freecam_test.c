// SPDX-License-Identifier: GPL-3.0-or-later
#define MOD_ID "pokemon-colosseum-freecam"
#define MOD_VERSION "1.0.0"
#define MOD_DISPLAY_NAME "Free Camera Test"
#include "../freecam_mod.c"
#include <stdlib.h>

static uint8_t locked_cache[0x4000];
static void *external_pointer(CPUState *s, uint32_t address, uint32_t size) {
  (void)s;
  uint32_t offset = address - 0xE0000000u;
  return offset < sizeof(locked_cache) && size <= sizeof(locked_cache) - offset
             ? locked_cache + offset
             : NULL;
}

// CHECK remains active in Release builds.
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static void put32(CPUState *s, uint32_t address, uint32_t value) {
  uint8_t *p = memory(s, address, 4);
  CHECK(p);
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void input(CPUState *s, unsigned buttons, int mx, int my, int cx,
                  int cy) {
  uint8_t *p = memory(s, 0x80002000, 12);
  memset(p, 0, 12);
  p[0] = (uint8_t)(buttons >> 8);
  p[1] = (uint8_t)buttons;
  p[2] = (uint8_t)mx;
  p[3] = (uint8_t)my;
  p[4] = (uint8_t)cx;
  p[5] = (uint8_t)cy;
  s->gpr[3] = 0x80002000;
  s->timebase++;
  pad_entry(s);
  pad_return(s);
}

static void render(CPUState *s) {
  update_entry(s);
  load_entry(s);
  load_return(s);
  update_return(s);
}

static void camera_export(const char *name, CPUState *s) {
  const ModernGekkoModDesc *mod = moderngekko_get_mod();
  for (uint32_t i = 0; i < mod->num_exports; i++) {
    if (!strcmp(mod->exports[i].name, name)) {
      mod->exports[i].function(s);
      return;
    }
  }
  CHECK(0); // Exercise the public descriptor, not only private callbacks.
}

int main(void) {
  CPUState s = {0};
  uint8_t *camera, original_eye[12], original_interest[12];
  float yaw, x, z, moved;
  s.ram_size = 0x01800000;
  s.ram = calloc(1, s.ram_size);
  s.external_pointer = external_pointer;
  CHECK(s.ram);
  CHECK(!memory(&s, 0x7FFFFFFF, 4));
  CHECK(!memory(&s, 0x81800000, 1));
  CHECK(!memory(&s, 0x817FFFFF, 12));
  CHECK(!memory(&s, 0xE0003FFF, 12));
  CHECK(axis(12) == 0 && axis((uint8_t)-12) == 0);
  CHECK(axis(80) == 1 && axis(127) == 1);
  CHECK(axis((uint8_t)-128) == -1);
  {
    CPUState host = {0}; // Settings exports must work without game RAM.
    CHECK(moderngekko_get_mod()->num_exports == 3);
    camera_export("moderngekko.camera.get_settings", &host);
    CHECK(host.gpr[3] == 100 && host.gpr[4] == 0);
    host.gpr[3] = 0;
    host.gpr[4] = 7;
    camera_export("moderngekko.camera.configure", &host);
    CHECK(host.gpr[3] == 1);
    camera_export("moderngekko.camera.get_settings", &host);
    CHECK(host.gpr[3] == 50 && host.gpr[4] == 1);
    host.gpr[3] = 201;
    host.gpr[4] = 0;
    camera_export("moderngekko.camera.configure", &host);
    camera_export("moderngekko.camera.get_settings", &host);
    CHECK(host.gpr[3] == 200 && host.gpr[4] == 0);
    unload();
    camera_export("moderngekko.camera.get_settings", &host);
    CHECK(host.gpr[3] == 100 && host.gpr[4] == 0);
  }
  put32(&s, 0x80478C40, 0x80003000);
  *memory(&s, 0x8047AD71, 1) = 1;
  put32(&s, 0x8047AD68, 1);
  put32(&s, 0x8047ACC8, 0x80004000);
  put32(&s, 0x80004000, 0x80005000);
  put32(&s, 0x8000500C, 42);
  put32(&s, 0x8047AA74, 0x80006000);
  put32(&s, 0x8000600C, 0x80007000);
  *memory(&s, 0x80007050, 1) = 1;
  put32(&s, 0x8047AA80, 0xE0000000);
  put32(&s, 0xE0000054, 2); // Retail 30fps: two 60Hz retraces.
  put32(&s, 0x8047AA94, 60);
  camera = memory(&s, 0x80006000, 0x128);
  camera[1] = 1;
  write_float(camera + 0x70, 10);
  write_float(camera + 0x74, 20);
  write_float(camera + 0x78, -100);
  write_float(camera + 0x100, 10);
  write_float(camera + 0x104, 20);
  write_float(camera + 0x108, 0);
  write_float(camera + 0xF8, 1);
  memcpy(original_eye, camera + 0x70, 12);
  memcpy(original_interest, camera + 0x100, 12);
  input(&s, 0x100, 0, 0, 0, 0); // Disabled input preserved.
  CHECK(memory(&s, 0x80002000, 12)[0] == 1);
  render(&s);
  CHECK(!cam.enabled && cam.camera == 0x80006000);
  CHECK(cam.orbit_ready);
  {
    CPUState host = {0};
    for (unsigned detached = 0; detached < 2; detached++) {
      for (unsigned inverted = 0; inverted < 2; inverted++) {
        for (unsigned percent = 50; percent <= 200; percent *= 2) {
          camera_export("moderngekko.camera.recenter", &host);
          CHECK(host.gpr[3] == 1 && !cam.enabled && !cam.initialized &&
                !cam.orbit_saved && !cam.orbit_ready && cam.orbit_zoom == 1);
          render(&s);
          if (detached) {
            input(&s, 0, 0, 0, 0, 0);
            input(&s, TOGGLE, 0, 0, 0, 0);
            render(&s);
          }
          host.gpr[3] = percent;
          host.gpr[4] = inverted;
          camera_export("moderngekko.camera.configure", &host);
          input(&s, 0, 0, 0, 80, 80);
          render(&s);
          float step = 2.0f * percent / 100.0f / 30.0f;
          CHECK(fabsf((detached ? cam.yaw : cam.orbit_yaw) + step) < .0001f);
          CHECK(fabsf((detached ? cam.pitch : cam.orbit_pitch) -
                      (inverted ? -step : step)) < .0001f);
          render(&s); // Settings must retain once-per-poll integration.
          CHECK(fabsf((detached ? cam.yaw : cam.orbit_yaw) + step) < .0001f);
        }
      }
    }
    camera_export("moderngekko.camera.recenter", &host);
    camera_export("moderngekko.camera.get_settings", &host);
    CHECK(host.gpr[3] == 200 &&
          host.gpr[4] == 1); // Reset preserves preferences.
    host.gpr[3] = 100;
    host.gpr[4] = 0;
    camera_export("moderngekko.camera.configure", &host);
    render(&s);
  }
  put32(&s, 0x8047AD68,
        0); // Fixed Pokemon Center cameras have no bank entries.
  render(&s);
  CHECK(cam.orbit_ready && cam.camera == 0x80006000);
  *memory(&s, 0x8047AD71, 1) = 0; // Non-field scenes still remain excluded.
  render(&s);
  CHECK(!cam.orbit_ready && !cam.camera);
  *memory(&s, 0x8047AD71, 1) = 1;
  render(&s);
  CHECK(cam.orbit_ready);
  {
    input(&s, 0x108, 20, 70, 0, 0); // A + zoom-in preserves normal walking.
    CHECK(memory(&s, 0x80002000, 12)[0] == 1);
    CHECK(memory(&s, 0x80002000, 12)[1] == 0);
    CHECK(memory(&s, 0x80002000, 12)[2] == 20);
    CHECK(memory(&s, 0x80002000, 12)[3] == 70);
    update_entry(&s);
    load_entry(&s);
    CHECK(fabsf(cam.orbit_zoom - (1 - 0.8f / 30)) < .0001f);
    CHECK(fabsf(read_float(camera + 0x78) + 100 * cam.orbit_zoom) < .001f);
    load_return(&s);
    update_return(&s);
    float zoom = cam.orbit_zoom;
    render(&s);
    CHECK(cam.orbit_zoom == zoom); // Multiple draw passes cannot zoom twice.
    input(&s, 4, 0, 0, 0, 0);
    render(&s);
    CHECK(fabsf(cam.orbit_zoom - 1) < .0001f);
    input(&s, 12, 0, 0, 0, 0); // Opposite zoom buttons cancel.
    render(&s);
    CHECK(fabsf(cam.orbit_zoom - 1) < .0001f);
    put32(&s, 0x8040836C, 1); // Menu navigation retains D-pad Up/Down.
    input(&s, 8, 0, 0, 0, 0);
    CHECK(memory(&s, 0x80002000, 12)[1] == 8);
    render(&s);
    CHECK(fabsf(cam.orbit_zoom - 1) < .0001f);
    put32(&s, 0x8040836C, 0);
    for (int i = 0; i < 100; i++) {
      input(&s, 8, 0, 0, 0, 0);
      render(&s);
    }
    CHECK(cam.orbit_zoom == .35f);
    for (int i = 0; i < 120; i++) {
      input(&s, 4, 0, 0, 0, 0);
      render(&s);
    }
    CHECK(cam.orbit_zoom == 2.5f);
    input(&s, RESET, 0, 0, 0, 0);
    render(&s);
    CHECK(cam.orbit_zoom == 1); // Recenter must win over zoom-out.
    put32(&s, 0xE0000054, 1);
    input(&s, 8, 0, 0, 0, 0);
    render(&s);
    CHECK(fabsf(cam.orbit_zoom - (1 - 0.8f / 60)) < .0001f);
    put32(&s, 0xE0000054, 2);
    input(&s, TOGGLE, 0, 0, 0, 0);
    render(&s);
    CHECK(cam.enabled);
    CHECK(fabsf(cam.z + 100 * cam.orbit_zoom) < .001f);
    zoom = cam.orbit_zoom;
    input(&s, 4, 0, 0, 0, 0);
    render(&s);
    CHECK(cam.orbit_zoom == zoom); // Detached flight retains orbit zoom.
    input(&s, TOGGLE, 0, 0, 0, 0);
    render(&s);
    CHECK(!cam.enabled && cam.orbit_zoom == zoom);
    input(&s, RESET, 0, 0, 0, 0);
    render(&s);
  }
  input(&s, 0x100, 20, 70, 80, 0);
  CHECK(memory(&s, 0x80002000, 12)[0] == 1);
  CHECK(memory(&s, 0x80002000, 12)[2] == 20);
  CHECK(memory(&s, 0x80002000, 12)[3] == 70);
  CHECK(memory(&s, 0x80002000, 12)[4] == 0);
  update_entry(&s);
  load_entry(&s);
  CHECK(cam.pending && cam.orbit_yaw < 0);
  CHECK(read_float(camera + 0x70) > 10);
  CHECK(read_float(camera + 0x100) == 10); // Orbit keeps the player centered.
  load_return(&s);
  update_return(&s);
  {
    float orbit = cam.orbit_yaw;
    write_float(memory(&s, 0x80003014, 4), 0.3f);
    s.lr = 0x80186FC8;
    heading_entry(&s);
    CHECK(cam.heading_pending);
    CHECK(fabsf(read_float(memory(&s, 0x80003014, 4)) -
                remainderf(orbit + 3.14159265f, 6.2831853f)) < 0.0001f);
    heading_entry(&s); // Duplicate alias callback must not overwrite backup.
    heading_return(&s);
    CHECK(read_float(memory(&s, 0x80003014, 4)) == 0.3f);
    s.lr = 0x801173E0; // Authored camera logic must not see the override.
    heading_entry(&s);
    CHECK(!cam.heading_pending);
    // Follow a moving player instead of leaving the orbit center behind.
    write_float(camera + 0x70, 15);
    write_float(camera + 0x100, 15);
    load_entry(&s);
    CHECK(read_float(camera + 0x100) == 15 && cam.orbit_yaw == orbit);
    load_return(&s);
    write_float(camera + 0x70, 10);
    write_float(camera + 0x100, 10);
    input(&s, TOGGLE, 0, 0, 0, 0);
    render(&s);
    CHECK(cam.enabled && cam.initialized);
    CHECK(fabsf(cam.x - (10 - sinf(orbit) * 100)) < .001f);
    s.lr = 0x80186FC8;
    heading_entry(&s);
    CHECK(!cam.heading_pending); // Detached flight never steers the player.
    input(&s, 0, 0, 0, 80, 0);
    render(&s);
    CHECK(cam.yaw != orbit && cam.orbit_yaw == orbit);
    input(&s, TOGGLE, 0, 0, 0, 0);
    render(&s);
    CHECK(!cam.enabled && cam.orbit_yaw == orbit);
  }
  input(&s, RESET, 0, 0, 0, 0);
  render(&s); // Recenter to the authored view before testing detached flight.
  CHECK(fabsf(cam.orbit_yaw) < 0.0001f);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  CHECK(cam.enabled);
  CHECK(read32(&s, 0x80002000) == 0);
  input(&s, TOGGLE, 0, 0, 0, 0); // Held chord must not toggle twice.
  CHECK(cam.enabled);
  render(&s);
  CHECK(cam.initialized && fabsf(cam.yaw) < 0.0001f);
  input(&s, 0, 0, 80, 80, 0);
  {
    unsigned sample = cam.sample;
    pad_return(&s); // A duplicate return must not re-read the cleared buffer.
    CHECK(cam.sample == sample && cam.move_y == 1 && cam.look_x == 1);
  }
  update_entry(&s);
  load_entry(&s);
  CHECK(cam.pending && read_float(camera + 0x78) > -100);
  yaw = cam.yaw;
  x = cam.x;
  z = cam.z;
  CHECK(yaw < 0 && x < 10); // C-right looks toward screen-right (-X).
  moved = sqrtf((x - 10) * (x - 10) + (z + 100) * (z + 100));
  CHECK(fabsf(moved - 250.0f / 30.0f) < 0.001f);
  load_return(&s);
  update_return(&s);
  CHECK(!memcmp(camera + 0x70, original_eye, 12));
  CHECK(!memcmp(camera + 0x100, original_interest, 12));
  CHECK(camera[2] == 1 && !cam.pending);
  load_entry(&s); // The renderer reloads the same camera outside update scope.
  CHECK(cam.pending && read_float(camera + 0x70) == cam.x);
  load_return(&s);
  render(&s); // Multiple renders per input poll don't integrate twice.
  CHECK(cam.yaw == yaw && cam.x == x && cam.z == z);
  put32(&s, 0xE0000054, 1); // 60fps same per-second speed.
  input(&s, 0, 0, 80, 0, 0);
  render(&s);
  CHECK(fabsf(sqrtf((cam.x - x) * (cam.x - x) + (cam.z - z) * (cam.z - z)) -
              250.0f / 60.0f) < 0.001f);
  {
    float y = cam.y;
    input(&s, 0x20, 0, 0, 0, 0);
    render(&s);
    CHECK(fabsf(cam.y - y - 250.0f / 60.0f) < 0.001f);
    input(&s, 0x40, 0, 0, 0, 0);
    render(&s);
    CHECK(fabsf(cam.y - y) < 0.001f);
    for (int i = 0; i < 60; i++) {
      input(&s, 0, 0, 0, 0, 80);
      render(&s);
    }
    CHECK(cam.pitch == 1.45f);
  }
  input(&s, RESET, 0, 0, 0, 0);
  CHECK(!cam.enabled);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  CHECK(cam.enabled);
  put32(&s, 0x8000500C, 43);
  render(&s);
  CHECK(!cam.enabled);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  *memory(&s, 0x80003000, 1) = 1;
  render(&s);
  CHECK(!cam.enabled && !cam.camera); // Scripted camera.
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  CHECK(!cam.enabled && memory(&s, 0x80002000, 12)[0] == 0);
  *memory(&s, 0x80003000, 1) = 0;
  render(&s);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  camera[4] = 1;
  render(&s);
  CHECK(!cam.enabled); // Animation.
  camera[4] = 0;
  render(&s);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, TOGGLE, 0, 0, 0, 0);
  memory(&s, 0x80002000, 12)[10] = 255;
  s.timebase++;
  pad_return(&s);
  CHECK(!cam.enabled); // Controller disconnect.
  reset_floor(&s);
  CHECK(!cam.camera);
  render(&s);
  input(&s, 8, 0, 0, 80, 30);
  render(&s);
  {
    float yaw = cam.orbit_yaw, pitch = cam.orbit_pitch;
    float zoom = cam.orbit_zoom, distance = cam.orbit_radius * zoom;
    reset_floor(&s); // Door begins unloading the old room.
    CHECK(cam.orbit_saved && !cam.orbit_ready && !cam.enabled);
    put32(&s, 0x80004000, 0);
    *memory(&s, 0x8047AD71, 1) = 0;
    *memory(&s, 0x80003000, 1) = 1;
    render(&s); // Loading/fade/scripted frames must not clear the saved view.
    CHECK(cam.orbit_saved && cam.orbit_yaw == yaw && cam.orbit_zoom == zoom);
    memory(&s, 0x80002000, 12)[10] =
        254; // Transient controller-not-ready poll.
    s.timebase++;
    pad_return(&s);
    CHECK(cam.orbit_saved && cam.orbit_yaw == yaw && cam.orbit_zoom == zoom);
    input(&s, 0, 0, 0, 0, 0);
    put32(&s, 0x80004000, 0x80005000);
    put32(&s, 0x8000500C, 44);
    *memory(&s, 0x8047AD71, 1) = 1;
    *memory(&s, 0x80003000, 1) = 0;
    put32(&s, 0x8047AA74, 0x80009000);
    put32(&s, 0x8000900C, 0x80007000);
    uint8_t *next = memory(&s, 0x80009000, 0x128);
    next[1] = 1;
    write_float(next + 0x70, 1000);
    write_float(next + 0x74, 120);
    write_float(next + 0x78, 1700);
    write_float(next + 0x100, 1000);
    write_float(next + 0x104, 60); // Destination ground is 40 units higher.
    write_float(next + 0x108, 2000);
    update_entry(&s);
    load_entry(&s);
    CHECK(cam.orbit_ready && cam.orbit_yaw == yaw && cam.orbit_pitch == pitch);
    CHECK(cam.orbit_zoom == zoom);
    float dx = read_float(next + 0x70) - 1000;
    float dy = read_float(next + 0x74) - 60;
    float dz = read_float(next + 0x78) - 2000;
    CHECK(fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - distance) < .001f);
    load_return(&s);
    update_return(&s);
    input(&s, RESET, 0, 0, 0, 0);
    render(&s);
    CHECK(cam.orbit_zoom == 1 && fabsf(cam.orbit_yaw) < .0001f);
    CHECK(cam.orbit_radius >
          300); // Explicit reset adopts the new authored view.
  }
  // A different city's authored orientation rotates the saved view offset.
  input(&s, 0, 0, 0, 80, 0); render(&s);
  float old_yaw = cam.orbit_yaw, old_pitch = cam.orbit_pitch;
  float old_radius = cam.orbit_radius, old_zoom = cam.orbit_zoom;
  reset_floor(&s);
  *memory(&s, 0x80005001, 1) = 2;
  put32(&s, 0x8000500C, 10);
  uint8_t *destination = memory(&s, 0x80009000, 0x128);
  write_float(destination + 0x78, 2300); // Facing reverses in the new area.
  render(&s);
  CHECK(fabsf(remainderf(cam.orbit_yaw - old_yaw - 3.14159265f,
                         6.2831853f)) < .0001f);
  CHECK(cam.orbit_pitch == old_pitch && cam.orbit_zoom == old_zoom);
  CHECK(cam.orbit_radius == old_radius && !cam.rebase_area);
  float new_yaw = cam.orbit_yaw;
  render(&s); CHECK(cam.orbit_yaw == new_yaw); // Rebase only once.
  input(&s, RESET, 0, 0, 0, 0);
  write_float(destination + 0x78, 1700);
  render(&s);
  // First-person chord is edge-triggered; normal walking survives release.
  input(&s, 0, 0, 0, 0, 0);
  input(&s, FIRST_PERSON, 0, 0, 0, 0);
  CHECK(cam.first_person && !cam.enabled);
  input(&s, FIRST_PERSON, 0, 0, 0, 0);
  CHECK(cam.first_person);
  uint8_t *player = memory(&s, 0x80452EE4, 12);
  write_float(player, 20); write_float(player + 4, 40);
  write_float(player + 8, 60);
  input(&s, 0x100, 0, 80, 0, 0);
  CHECK(memory(&s, 0x80002000, 12)[0] == 1);
  CHECK(memory(&s, 0x80002000, 12)[3] == 80);
  write_float(memory(&s, 0x80007038, 4), 20);
  update_entry(&s); load_entry(&s);
  CHECK(read_float(memory(&s, 0x80007038, 4)) == 1);
  uint8_t *view = memory(&s, cam.camera, 0x128);
  CHECK(fabsf(read_float(view + 0x74) - 54) < .001f);
  CHECK(fabsf(read_float(view + 0x78) - 62) < .001f);
  load_return(&s); update_return(&s);
  CHECK(read_float(memory(&s, 0x80007038, 4)) == 20);
  reset_floor(&s); render(&s);
  CHECK(cam.first_person && cam.orbit_ready);
  input(&s, FIRST_PERSON, 0, 0, 0, 0);
  CHECK(!cam.first_person);
  input(&s, 0, 0, 0, 0, 0);
  input(&s, FIRST_PERSON, 0, 0, 0, 0);
  input(&s, RESET, 0, 0, 0, 0);
  CHECK(!cam.first_person);
  // Head-only skinning override is scoped to Wes and restored after recursion.
  put32(&s, 0x8047AC5C, 0x8000A000);
  put32(&s, 0x8047AC60, 1); put32(&s, 0x8047D030, 100);
  put32(&s, 0x8000A004, 0x8000B000);
  put32(&s, 0x8000A00C, 100);
  put32(&s, 0x8000B008, 0x8000C000);
  put32(&s, 0x8000C010, 0x8000C100);
  put32(&s, 0x8000C118, 0x8000D000);
  put32(&s, 0x8000D00C, 0x8000D100);
  put32(&s, 0x8000D114, 0x8000D200);
  put32(&s, 0x8000D204, 0x8000D300);
  put32(&s, 0x8000D304, 0x8000E000);
  put32(&s, 0x8000D308, 0x3F800000);
  uint8_t *head = memory(&s, 0x8000E000, 0x80);
  write_float(head + 0x60, 13);
  for (int i = 0; i < 3; i++) write_float(head + 0x2C + i * 4, 1);
  put32(&s, 0x8000E074, 0x8000E100);
  write_float(memory(&s, 0x8000E100, 12), 2);
  cam.first_person = cam.orbit_ready = 1;
  s.gpr[3] = 0x8000B000;
  head_entry(&s);
  CHECK(cam.head_depth == 1 && read_float(head + 0x2C) == .001f);
  s.gpr[3] = 0x8000C100; head_entry(&s); head_return(&s);
  CHECK(cam.head_depth == 1 && read_float(head + 0x2C) == .001f);
  write_float(memory(&s, 0x8000E100, 12), .002f);
  head_return(&s);
  CHECK(read_float(memory(&s, 0x8000E100, 12)) == 2);
  CHECK(!cam.head_depth && read_float(head + 0x2C) == 1);
  cam.first_person = 0; s.gpr[3] = 0x8000B000; head_entry(&s);
  CHECK(!cam.head_depth);
  cam.first_person = 1; s.gpr[3] = 0x8000C100; head_entry(&s);
  CHECK(!cam.head_depth); // NPC or another root cannot be modified.
  s.gpr[3] = 0x3010;
  head_all_entry(&s);
  CHECK(cam.head_depth == 1 && s.gpr[3] == 0x3010);
  CHECK(read_float(head + 0x2C) == .001f);
  head_return(&s);
  CHECK(read_float(head + 0x2C) == 1);
  write_float(memory(&s, 0x8000B018, 12), 20);
  write_float(memory(&s, 0x8000B01C, 4), 100);
  write_float(memory(&s, 0x8000B020, 4), 60);
  update_entry(&s); load_entry(&s);
  CHECK(fabsf(read_float(memory(&s, cam.camera + 0x74, 4)) - 114) < .001f);
  load_return(&s); update_return(&s);
  // First-person arrivals face the destination spawn heading, level.
  cam.orbit_yaw = -2; cam.orbit_pitch = -.7f;
  write_float(memory(&s, 0x8000B028, 4), 1.2f);
  float retained_zoom = cam.orbit_zoom;
  reset_floor(&s);
  CHECK(cam.entrance_pending);
  *memory(&s, 0x80003000, 1) = 1;
  render(&s); CHECK(cam.entrance_pending); // Keep pending through a fade.
  *memory(&s, 0x80003000, 1) = 0;
  put32(&s, 0x8000500C, 11);
  input(&s, 0, 0, 0, 80, 80);
  render(&s);
  CHECK(fabsf(cam.orbit_yaw - 1.2f) < .0001f && cam.orbit_pitch == 0);
  CHECK(!cam.entrance_pending && cam.orbit_zoom == retained_zoom);
  input(&s, 0, 0, 0, 80, 0); render(&s);
  CHECK(cam.orbit_yaw < 1.2f); // Looking stays free after arrival.
  input(&s, 0x0C00, 0, 0, 0, 0);
  CHECK(memory(&s, 0x80002000, 12)[0] == 0);
  cam.camera = cam.orbit_ready = 0;
  input(&s, 0x0D00, 0, 0, 0, 0);
  CHECK(memory(&s, 0x80002000, 12)[0] == 1); // A survives; X/Y never reach menus.
  CHECK(moderngekko_get_mod()->num_hooks == 13);
  free(s.ram);
  puts("freecam tests passed");
  return 0;
}
