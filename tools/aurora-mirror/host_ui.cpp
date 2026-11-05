// colosseum-standalone's window handling and pause menu (see host_ui.h).
#include "host_ui.h"

#include "audio_out.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
void aurora_native_set_overlay(void (*draw)(bool imgui_frame),
                               void (*snap)(uint8_t* rgb, uint32_t width, uint32_t height));
void aurora_native_request_quit();
}

namespace {

std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_toggle_fullscreen{false};
std::atomic<bool> g_save_requested{false};
std::atomic<int> g_saved_state{0};  // 0 none, 1 saved, -1 failed
std::atomic<bool> g_fullscreen{false};
std::atomic<int> g_window_width{1280}, g_window_height{960};
std::atomic<int> g_volume{100};
std::atomic<bool> g_pause_on_focus_loss{false};
std::atomic<int> g_aspect{2};  // 0 4:3, 1 16:9, 2 auto
std::atomic<bool> g_hd_text{false};
std::atomic<int> g_frame_rate{0}, g_display_rate{60};
constexpr const char* kFrameRates[] = {"native", "60", "120", "display"};
std::atomic<float> g_window_aspect{4.0f / 3.0f};
constexpr const char* kAspectNames[] = {"4:3", "16:9", "auto"};
bool g_start_fullscreen = false;  // render thread from here down

SDL_Window* game_window() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

// Render thread, inside SDL's event pump.
bool SDLCALL event_watch(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_KEY_DOWN:
    if (event->key.repeat)
      break;
    if (event->key.scancode == SDL_SCANCODE_ESCAPE)
      g_menu_open = !g_menu_open;
    else if (event->key.scancode == SDL_SCANCODE_F11 ||
             ((event->key.scancode == SDL_SCANCODE_RETURN || event->key.scancode == SDL_SCANCODE_KP_ENTER) &&
              (event->key.mod & SDL_KMOD_ALT)))
      g_toggle_fullscreen = true;
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
      g_menu_open = !g_menu_open;
    else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST && g_menu_open)
      g_menu_open = false;  // B backs out of the menu
    break;
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    if (g_pause_on_focus_loss)
      g_menu_open = true;
    break;
  default:
    break;
  }
  return true;
}

// What the menu drew in its last frame, for snapshots (render thread).
struct DrawCopy {
  std::vector<ImDrawVert> vertices;
  std::vector<ImDrawIdx> indices;
  std::vector<ImDrawCmd> commands;
  ImVec2 display{};
  bool valid = false;
} g_copy;

void copy_draw_list(const ImDrawList* list) {
  g_copy.vertices.assign(list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
  g_copy.indices.assign(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
  g_copy.commands.assign(list->CmdBuffer.Data, list->CmdBuffer.Data + list->CmdBuffer.Size);
  g_copy.display = ImGui::GetIO().DisplaySize;
  g_copy.valid = true;
}

void draw_menu(bool just_opened) {
  ImGuiIO& io = ImGui::GetIO();
  const float scale = std::max(1.0f, io.DisplaySize.y / 600.0f);
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  if (just_opened)
    ImGui::SetNextWindowFocus();
  ImGui::Begin("Paused", nullptr,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  ImGui::SetWindowFontScale(scale);
  const float width = 260.0f * scale;
  if (ImGui::Button("Resume", ImVec2(width, 0)))
    g_menu_open = false;
  if (just_opened)
    ImGui::SetItemDefaultFocus();
  bool fullscreen = g_fullscreen;
  if (ImGui::Checkbox("Fullscreen  (F11, Alt+Enter)", &fullscreen))
    g_toggle_fullscreen = true;
  int volume = g_volume;
  ImGui::SetNextItemWidth(width * 0.7f);
  if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%")) {
    g_volume = volume;
    audio_out_set_volume(float(volume) / 100.0f);
  }
  bool focus = g_pause_on_focus_loss;
  if (ImGui::Checkbox("Pause when the window loses focus", &focus))
    g_pause_on_focus_loss = focus;
  // Hor+ widescreen: 16:9, or the window's own shape (21:9 on an ultrawide).
  int aspect = g_aspect;
  ImGui::SetNextItemWidth(width * 0.7f);
  if (ImGui::Combo("Aspect ratio", &aspect, "4:3\0" "16:9\0" "Auto (window)\0"))
    g_aspect = aspect;
  int rate = g_frame_rate;
  ImGui::SetNextItemWidth(width * 0.7f);
  if (ImGui::Combo("Frame rate", &rate, "Native\0" "60\0" "120\0" "Display (up to 240)\0"))
    g_frame_rate = rate;
  ImGui::TextDisabled("Guest timing stays 59.94 Hz");
  bool hd_text = g_hd_text;
  if (ImGui::Checkbox("HD text", &hd_text))
    g_hd_text = hd_text;
  if (ImGui::Button("Save settings", ImVec2(width, 0))) {
    g_saved_state = 0;
    g_save_requested = true;
  }
  if (const int saved = g_saved_state; saved != 0) {
    ImGui::SameLine();
    ImGui::TextUnformatted(saved > 0 ? "Saved" : "Could not save");
  }
  if (ImGui::Button("Quit", ImVec2(width, 0))) {
    g_menu_open = false;
    aurora_native_request_quit();
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Esc / Back: resume");
  copy_draw_list(ImGui::GetWindowDrawList());
  ImGui::End();
}

// Render thread, before each present.
void overlay_draw(bool imgui_frame) {
  static bool first = true, was_open = false;
  SDL_Window* window = game_window();
  if (first && window) {
    first = false;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing of ours to remember; keep Aurora's folder as it is
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (g_start_fullscreen)
      SDL_SetWindowFullscreen(window, true);
  }
  if (window && g_toggle_fullscreen.exchange(false))
    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
  if (window) {
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(window, &pw, &ph) && pw > 0 && ph > 0)
      g_window_aspect = float(pw) / float(ph);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
      if (mode->refresh_rate > 0)
        g_display_rate = int(std::round(mode->refresh_rate));
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    g_fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (!(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
      int w = 0, h = 0;
      if (SDL_GetWindowSize(window, &w, &h) && w >= 320 && h >= 240) {
        g_window_width = w;
        g_window_height = h;
      }
    }
  }
  g_copy.valid = false;
  const bool open = g_menu_open;
  if (open && imgui_frame)
    draw_menu(!was_open);
  was_open = open && imgui_frame;
}

// Paints the menu as ImGui drew it over a snapshot of the game image (which
// the window shows letterboxed): the overlay itself is drawn after the
// renderer's readback point. Render thread.
void overlay_snap(uint8_t* rgb, uint32_t width, uint32_t height) {
  if (!g_copy.valid || g_copy.display.x <= 0 || g_copy.display.y <= 0)
    return;
  const float s = std::min(g_copy.display.x / float(width), g_copy.display.y / float(height));
  const float ox = (g_copy.display.x - float(width) * s) * 0.5f;
  const float oy = (g_copy.display.y - float(height) * s) * 0.5f;
  for (uint32_t i = 0; i < width * height * 3; ++i)  // the background dim, alpha 150
    rgb[i] = uint8_t(rgb[i] * (255 - 150) / 255);
  unsigned char* tex = nullptr;
  int tw = 0, th = 0;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
  if (!tex)
    return;
  for (const ImDrawCmd& cmd : g_copy.commands) {
    if (cmd.UserCallback || cmd.ElemCount == 0)
      continue;
    const float cx0 = std::max(0.0f, (cmd.ClipRect.x - ox) / s), cy0 = std::max(0.0f, (cmd.ClipRect.y - oy) / s);
    const float cx1 = std::min(float(width), (cmd.ClipRect.z - ox) / s);
    const float cy1 = std::min(float(height), (cmd.ClipRect.w - oy) / s);
    for (uint32_t e = 0; e + 2 < cmd.ElemCount; e += 3) {
      const ImDrawVert* v[3];
      float x[3], y[3];
      for (int k = 0; k < 3; ++k) {
        v[k] = &g_copy.vertices[cmd.VtxOffset + g_copy.indices[cmd.IdxOffset + e + k]];
        x[k] = (v[k]->pos.x - ox) / s;
        y[k] = (v[k]->pos.y - oy) / s;
      }
      const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
      if (std::fabs(area) < 1e-6f)
        continue;
      const int px0 = int(std::max(cx0, std::floor(std::min({x[0], x[1], x[2]}))));
      const int py0 = int(std::max(cy0, std::floor(std::min({y[0], y[1], y[2]}))));
      const int px1 = int(std::min(cx1, std::ceil(std::max({x[0], x[1], x[2]}))));
      const int py1 = int(std::min(cy1, std::ceil(std::max({y[0], y[1], y[2]}))));
      for (int py = py0; py < py1; ++py) {
        for (int px = px0; px < px1; ++px) {
          const float fx = px + 0.5f, fy = py + 0.5f;
          const float w0 = ((x[1] - fx) * (y[2] - fy) - (x[2] - fx) * (y[1] - fy)) / area;
          const float w1 = ((x[2] - fx) * (y[0] - fy) - (x[0] - fx) * (y[2] - fy)) / area;
          const float w2 = 1.0f - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0)
            continue;
          const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
          const float t = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
          const int tx = std::clamp(int(u * tw), 0, tw - 1), ty = std::clamp(int(t * th), 0, th - 1);
          const unsigned char* texel = tex + (size_t(ty) * tw + tx) * 4;
          float color[4];
          for (int c = 0; c < 4; ++c)
            color[c] = (w0 * ((v[0]->col >> (8 * c)) & 0xFF) + w1 * ((v[1]->col >> (8 * c)) & 0xFF) +
                        w2 * ((v[2]->col >> (8 * c)) & 0xFF)) *
                       texel[c] / 255.0f;
          const float a = color[3] / 255.0f;
          uint8_t* dst = rgb + (size_t(py) * width + px) * 3;
          for (int c = 0; c < 3; ++c)
            dst[c] = uint8_t(std::clamp(dst[c] * (1.0f - a) + color[c] * a, 0.0f, 255.0f));
        }
      }
    }
  }
}

}  // namespace

void host_ui_init(const HostSettings& settings) {
  g_start_fullscreen = settings.fullscreen;
  g_fullscreen = settings.fullscreen;
  g_window_width = settings.window_width;
  g_window_height = settings.window_height;
  g_volume = settings.volume;
  g_pause_on_focus_loss = settings.pause_on_focus_loss;
  g_aspect = settings.aspect == "4:3" ? 0 : settings.aspect == "16:9" ? 1 : 2;
  g_hd_text = settings.hd_text;
  g_frame_rate = settings.frame_rate == "60" ? 1 : settings.frame_rate == "120" ? 2 : settings.frame_rate == "display" ? 3 : 0;
  if (settings.window_height > 0)
    g_window_aspect = float(settings.window_width) / float(settings.window_height);
  aurora_native_set_overlay(overlay_draw, overlay_snap);
}

void host_ui_start() { SDL_AddEventWatch(event_watch, nullptr); }

bool host_ui_menu_open() { return g_menu_open; }
void host_ui_set_menu(bool open) { g_menu_open = open; }
bool host_ui_take_save_request() { return g_save_requested.exchange(false); }
void host_ui_saved(bool ok) { g_saved_state = ok ? 1 : -1; }

void host_ui_store(HostSettings* settings) {
  settings->fullscreen = g_fullscreen;
  settings->window_width = g_window_width;
  settings->window_height = g_window_height;
  settings->volume = g_volume;
  settings->pause_on_focus_loss = g_pause_on_focus_loss;
  settings->aspect = kAspectNames[std::clamp(g_aspect.load(), 0, 2)];
  settings->hd_text = g_hd_text;
  settings->frame_rate = kFrameRates[std::clamp(g_frame_rate.load(), 0, 3)];
}

int host_ui_frame_rate() {
  const int choice = g_frame_rate;
  return choice == 0 ? 0 : choice == 2 ? 120 : choice == 3 ? std::clamp(g_display_rate.load(), 60, 240) : 60;
}
int host_ui_aspect() { return g_aspect; }
bool host_ui_hd_text() { return g_hd_text; }
float host_ui_window_aspect() { return g_window_aspect; }
