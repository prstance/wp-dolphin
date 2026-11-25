// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/WPOverlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <picojson.h>

#include "Common/Buffer.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

namespace WPOverlay
{
namespace
{
using Clock = std::chrono::steady_clock;

// The loading screen fades out in this long, the notifications slide in and out in this long.
constexpr double FADE = 0.6;
constexpr double SLIDE = 0.35;
// Without the game telling where it is (another title than Wii Party), the loading screen goes this
// long after the game's first frame; Wii Party says it, this is only in case it doesn't.
constexpr double OTHER_TITLE = 2.5;
constexpr double WII_PARTY = 40.0;
// At most this many notifications at once; the others wait their turn.
constexpr std::size_t SHOWN = 4;
// While loading, a frame at least this often (seconds).
constexpr double IDLE_FRAME = 1.0 / 30;

struct Look
{
  ImU32 bg = IM_COL32(0xef, 0xee, 0xef, 0xff);
  ImU32 card = IM_COL32(0xff, 0xff, 0xff, 0xff);
  ImU32 text = IM_COL32(0x3c, 0x3c, 0x3c, 0xff);
  ImU32 muted = IM_COL32(0x6b, 0x64, 0x68, 0xff);
  ImU32 accent = IM_COL32(0xe6, 0x32, 0x8c, 0xff);
  ImU32 accent_soft = IM_COL32(0xea, 0x66, 0xa2, 0xff);
  ImU32 accent_deep = IM_COL32(0xa3, 0x28, 0x6a, 0xff);
  ImU32 label = IM_COL32(0x78, 0x00, 0x34, 0xff);
  ImU32 tint = IM_COL32(0xfc, 0xe4, 0xea, 0xff);
  ImU32 line = IM_COL32(0xe3, 0xe0, 0xe2, 0xff);
  ImU32 decor = IM_COL32(0xfc, 0xe4, 0xea, 0xff);
  ImU32 grad_top = IM_COL32(0xf8, 0xb4, 0xd8, 0xff);
  ImU32 grad_bottom = IM_COL32(0xec, 0x70, 0xb4, 0xff);
  ImU32 shadow = IM_COL32(0x78, 0x00, 0x34, 0xff);
  ImU32 danger_bg = IM_COL32(0xfd, 0xe0, 0xe0, 0xff);
  ImU32 danger = IM_COL32(0x7a, 0x14, 0x24, 0xff);
  ImU32 warn = IM_COL32(0xd9, 0x8b, 0x1c, 0xff);
  ImU32 ok = IM_COL32(0x25, 0xa7, 0x20, 0xff);
  std::array<ImU32, 4> players = {IM_COL32(0x12, 0x83, 0xcb, 0xff), IM_COL32(0xd7, 0x5d, 0x41, 0xff),
                                  IM_COL32(0x25, 0xa7, 0x20, 0xff), IM_COL32(0xd9, 0xb5, 0x27, 0xff)};
};

struct Config
{
  bool enabled = false;
  Look look;
  std::string title;
  std::string subtitle;
  std::string logo;
  // The loading screen's line: the game starting, the shaders compiling ("{n}": their percentage).
  std::string starting;
  std::string shaders;
};

struct Row
{
  int player = 0;
  std::string badge;
  std::string label;
  std::string value;
  std::optional<int> battery;
};

struct Toast
{
  std::string icon;
  bool warning = false;
  std::string title;
  std::string text;
  std::vector<Row> rows;
  double seconds = 5;
  // When it came on screen (-1: waiting), and where it is in the stack (it moves to its place).
  double shown_at = -1;
  float y = 0;
};

std::string Text(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  return it != object.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

std::optional<double> Number(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<double>())
    return std::nullopt;
  return it->second.get<double>();
}

// "#rrggbb", or the colour it had.
void Colour(const picojson::object& object, const char* key, ImU32* colour)
{
  const std::string hex = Text(object, key);
  if (hex.size() != 7 || hex[0] != '#')
    return;
  char* end = nullptr;
  const unsigned long rgb = std::strtoul(hex.c_str() + 1, &end, 16);
  if (end != hex.c_str() + 7)
    return;
  *colour = IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
}

Config Load()
{
  Config config;
  const char* env = std::getenv("WP_OVERLAY");
  if (!env)
    return config;
  config.enabled = true;
  picojson::value root;
  if (!picojson::parse(root, env).empty() || !root.is<picojson::object>())
    return config;
  const auto& object = root.get<picojson::object>();
  config.title = Text(object, "title");
  config.subtitle = Text(object, "subtitle");
  config.logo = Text(object, "logo");
  config.starting = Text(object, "starting");
  config.shaders = Text(object, "shaders");
  const auto look = object.find("look");
  if (look != object.end() && look->second.is<picojson::object>())
  {
    const auto& l = look->second.get<picojson::object>();
    Look& c = config.look;
    Colour(l, "bg", &c.bg);
    Colour(l, "card", &c.card);
    Colour(l, "text", &c.text);
    Colour(l, "muted", &c.muted);
    Colour(l, "accent", &c.accent);
    Colour(l, "accentSoft", &c.accent_soft);
    Colour(l, "accentDeep", &c.accent_deep);
    Colour(l, "label", &c.label);
    Colour(l, "tint", &c.tint);
    Colour(l, "line", &c.line);
    Colour(l, "decor", &c.decor);
    Colour(l, "gradTop", &c.grad_top);
    Colour(l, "gradBottom", &c.grad_bottom);
    Colour(l, "shadow", &c.shadow);
    Colour(l, "dangerBg", &c.danger_bg);
    Colour(l, "danger", &c.danger);
    Colour(l, "warn", &c.warn);
    Colour(l, "ok", &c.ok);
    const auto players = l.find("players");
    if (players != l.end() && players->second.is<picojson::array>())
    {
      const auto& list = players->second.get<picojson::array>();
      for (std::size_t i = 0; i < std::min(list.size(), c.players.size()); ++i)
      {
        picojson::object one;
        one["c"] = list[i];
        Colour(one, "c", &c.players[i]);
      }
    }
  }
  return config;
}

const Config& Get()
{
  static const Config config = Load();
  return config;
}

double Now()
{
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// From stdin's thread to the video thread.
std::mutex s_incoming_mutex;
std::deque<Toast> s_incoming;

std::atomic<bool> s_shown{false};
std::atomic<double> s_first_frame{-1};
std::atomic<std::size_t> s_shaders_done{0};
std::atomic<std::size_t> s_shaders_total{0};
// The loading screen is gone, faded out.
std::atomic<bool> s_gone{false};

// The video thread's.
std::vector<Toast> s_toasts;
double s_loaded_at = -1;
double s_last_draw = -1;
ImFont* s_bold = nullptr;
ImFont* s_extra_bold = nullptr;
std::unique_ptr<AbstractTexture> s_logo;
bool s_logo_tried = false;

void Receive(const std::string& line)
{
  picojson::value root;
  if (!picojson::parse(root, line).empty() || !root.is<picojson::object>())
    return;
  const auto& object = root.get<picojson::object>();
  Toast toast;
  toast.icon = Text(object, "icon");
  toast.warning = Text(object, "tone") == "warning";
  toast.title = Text(object, "title");
  toast.text = Text(object, "text");
  toast.seconds = std::clamp(Number(object, "seconds").value_or(5.0), 1.0, 30.0);
  const auto rows = object.find("rows");
  if (rows != object.end() && rows->second.is<picojson::array>())
  {
    for (const auto& value : rows->second.get<picojson::array>())
    {
      if (!value.is<picojson::object>())
        continue;
      const auto& r = value.get<picojson::object>();
      Row row;
      row.player = std::clamp(static_cast<int>(Number(r, "player").value_or(0)), 0, 3);
      row.badge = Text(r, "badge");
      row.label = Text(r, "label");
      row.value = Text(r, "value");
      if (const auto battery = Number(r, "battery"))
        row.battery = std::clamp(static_cast<int>(*battery), 0, 100);
      toast.rows.push_back(std::move(row));
    }
  }
  if (toast.title.empty() && toast.text.empty() && toast.rows.empty())
    return;
  std::lock_guard lock(s_incoming_mutex);
  s_incoming.push_back(std::move(toast));
}

// The launcher's lines, for as long as Dolphin runs.
void ReadStdin()
{
  std::string pending;
  char buffer[4096];
  for (;;)
  {
#ifdef _WIN32
    DWORD n = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &n, nullptr) || n == 0)
      return;
#else
    const ssize_t n = read(0, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return;
#endif
    pending.append(buffer, static_cast<std::size_t>(n));
    for (std::size_t end; (end = pending.find('\n')) != std::string::npos;)
    {
      Receive(pending.substr(0, end));
      pending.erase(0, end + 1);
    }
  }
}

// Drawing --------------------------------------------------------------------------------------

ImU32 Fade(ImU32 colour, float alpha)
{
  const u32 a = static_cast<u32>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.f, 1.f));
  return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

float EaseOut(double t)
{
  const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
  return 1 - (1 - x) * (1 - x) * (1 - x);
}

ImFont* Bold()
{
  return s_bold ? s_bold : ImGui::GetFont();
}

ImFont* ExtraBold()
{
  return s_extra_bold ? s_extra_bold : Bold();
}

ImVec2 Measure(ImFont* font, float size, const std::string& text)
{
  return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str(), text.c_str() + text.size());
}

// The text cut into lines no wider than `width`, between words.
std::vector<std::string> Wrap(ImFont* font, float size, const std::string& text, float width)
{
  std::vector<std::string> lines;
  std::string line;
  std::size_t start = 0;
  while (start <= text.size())
  {
    std::size_t end = text.find(' ', start);
    if (end == std::string::npos)
      end = text.size();
    const std::string word = text.substr(start, end - start);
    const std::string longer = line.empty() ? word : line + ' ' + word;
    if (!line.empty() && Measure(font, size, longer).x > width)
    {
      lines.push_back(line);
      line = word;
    }
    else
    {
      line = longer;
    }
    start = end + 1;
  }
  if (!line.empty())
    lines.push_back(line);
  return lines;
}

void Write(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 colour, const std::string& text)
{
  dl->AddText(font, size, pos, colour, text.c_str(), text.c_str() + text.size());
}

// The launcher's titles: white letters in a thick outline.
void WriteOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 fill, ImU32 outline,
                   float width, const std::string& text)
{
  constexpr int STEPS = 16;
  for (int i = 0; i < STEPS; ++i)
  {
    const float angle = 2 * IM_PI * i / STEPS;
    Write(dl, font, size, {pos.x + std::cos(angle) * width, pos.y + std::sin(angle) * width}, outline,
          text);
  }
  Write(dl, font, size, pos, fill, text);
}

// Bilinear filtering for what's drawn after it (the logo, the text as it slides), point after.
void Linear(const ImDrawList*, const ImDrawCmd*)
{
  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
}

void Point(const ImDrawList*, const ImDrawCmd*)
{
  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
}

void LoadLogo()
{
  s_logo_tried = true;
  std::string file;
  if (Get().logo.empty() || !File::ReadFileToString(Get().logo, file))
    return;
  Common::UniqueBuffer<u8> pixels;
  u32 width = 0, height = 0;
  if (!Common::LoadPNG(std::span(reinterpret_cast<const u8*>(file.data()), file.size()), &pixels,
                       &width, &height))
    return;
  const TextureConfig config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                             AbstractTextureType::Texture_2DArray);
  s_logo = g_gfx->CreateTexture(config, "Wii Party launcher logo");
  if (s_logo)
    s_logo->Load(0, width, height, width, pixels.data(), sizeof(u32) * width * height);
}

// The launcher's soft shadow under a card.
void Shadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, float u, float alpha)
{
  for (int i = 1; i <= 6; ++i)
  {
    const float grow = i * 3 * u;
    dl->AddRectFilled({p0.x - grow, p0.y - grow + 10 * u}, {p1.x + grow, p1.y + grow + 10 * u},
                      Fade(Get().look.shadow, 0.03f * alpha), rounding + grow);
  }
}

// One of the launcher's icons, drawn on its 24 × 24 grid around `centre`.
void Icon(ImDrawList* dl, const std::string& name, ImVec2 centre, float size, ImU32 colour)
{
  const float s = size / 24;
  const auto at = [&](float x, float y) { return ImVec2(centre.x + (x - 12) * s, centre.y + (y - 12) * s); };
  const float stroke = 2.4f * s;
  if (name == "remotes")
  {
    dl->AddRect(at(8, 2), at(16, 22), colour, 3 * s, 0, stroke);
    dl->AddLine(at(12, 5.5f), at(12, 9.5f), colour, stroke);
    dl->AddLine(at(10, 7.5f), at(14, 7.5f), colour, stroke);
    dl->AddCircleFilled(at(12, 14), 1.5f * s, colour);
    dl->AddLine(at(10.5f, 18), at(13.5f, 18), colour, stroke);
  }
  else if (name == "battery")
  {
    dl->AddRect(at(2.5f, 7), at(19, 17), colour, 3 * s, 0, stroke);
    dl->AddRectFilled(at(20.2f, 10), at(22.2f, 14), colour, 1 * s);
    dl->AddRectFilled(at(5.5f, 10), at(9, 14), colour, 1 * s);
  }
  else if (name == "pointer")
  {
    // A target.
    dl->AddCircle(centre, 7 * s, colour, 0, stroke);
    dl->AddCircleFilled(centre, 2 * s, colour);
    dl->AddLine(at(12, 1.5f), at(12, 5), colour, stroke);
    dl->AddLine(at(12, 19), at(12, 22.5f), colour, stroke);
    dl->AddLine(at(1.5f, 12), at(5, 12), colour, stroke);
    dl->AddLine(at(19, 12), at(22.5f, 12), colour, stroke);
  }
  else if (name == "mods")
  {
    // The launcher's Library: three layers.
    const ImVec2 top[] = {at(12, 3), at(21, 8), at(12, 13), at(3, 8)};
    dl->AddPolyline(top, 4, colour, ImDrawFlags_Closed, stroke);
    const ImVec2 middle[] = {at(3, 12.5f), at(12, 17.5f), at(21, 12.5f)};
    dl->AddPolyline(middle, 3, colour, ImDrawFlags_None, stroke);
    const ImVec2 bottom[] = {at(3, 17), at(12, 22), at(21, 17)};
    dl->AddPolyline(bottom, 3, colour, ImDrawFlags_None, stroke);
  }
  else if (name == "save")
  {
    // A floppy disk.
    dl->AddRect(at(4, 4), at(20, 20), colour, 2.5f * s, 0, stroke);
    dl->AddRect(at(8, 4), at(16, 9), colour, 1 * s, 0, stroke);
    dl->AddRectFilled(at(8, 13.5f), at(16, 17.5f), colour, 1 * s);
  }
  else if (name == "mii")
  {
    // The launcher's Miis: a head and shoulders.
    dl->AddCircle(at(12, 9), 5 * s, colour, 0, stroke);
    dl->PathArcTo(at(12, 21), 8 * s, IM_PI, 2 * IM_PI);
    dl->PathStroke(colour, ImDrawFlags_None, stroke);
  }
  else if (name == "clock")
  {
    dl->AddCircle(centre, 9.5f * s, colour, 0, stroke);
    dl->AddLine(centre, at(12, 6.5f), colour, stroke);
    dl->AddLine(centre, at(16, 14), colour, stroke);
  }
  else
  {
    dl->AddCircle(centre, 9.5f * s, colour, 0, stroke);
    dl->AddLine(at(12, 11), at(12, 16.5f), colour, stroke);
    dl->AddCircleFilled(at(12, 7.6f), 1.5f * s, colour);
  }
}

// A battery gauge, `width` wide, its level coloured by what's left.
void Gauge(ImDrawList* dl, ImVec2 pos, float width, int level, float u, float alpha)
{
  const Look& look = Get().look;
  const float height = width * 0.5f;
  const float stroke = 2.6f * u;
  const ImVec2 p1 = {pos.x + width - 4 * u, pos.y + height};
  dl->AddRect(pos, p1, Fade(look.muted, alpha), 6 * u, 0, stroke);
  dl->AddRectFilled({p1.x + 1 * u, pos.y + height * 0.3f}, {p1.x + 4 * u, pos.y + height * 0.7f},
                    Fade(look.muted, alpha), 1.5f * u);
  const ImU32 fill = level < 15 ? look.danger : level < 35 ? look.warn : look.ok;
  const float inset = stroke + 2 * u;
  const float full = p1.x - pos.x - 2 * inset;
  if (level > 0)
  {
    dl->AddRectFilled({pos.x + inset, pos.y + inset},
                      {pos.x + inset + std::max(full * level / 100.f, 3 * u), p1.y - inset},
                      Fade(fill, alpha), 3 * u);
  }
}

void DrawLoading(ImDrawList* dl, ImVec2 size, float u, float alpha, double now)
{
  const Config& config = Get();
  const Look& look = config.look;
  const float w = size.x, h = size.y;
  dl->AddRectFilled({0, 0}, size, Fade(look.bg, alpha));
  // The launcher's rings, behind.
  dl->AddCircle({w * 0.5f - h * 0.62f, -h * 0.1f}, h * 0.42f, Fade(look.decor, alpha), 0, h * 0.075f);
  dl->AddCircle({w * 0.5f + h * 0.72f, h * 0.96f}, h * 0.24f, Fade(look.decor, alpha), 0, h * 0.045f);

  const float cx = w / 2;
  const float logo = 170 * u;
  const float title_size = 74 * u;
  const float subtitle_size = 30 * u;
  const auto subtitle = Wrap(Bold(), subtitle_size, config.subtitle, 980 * u);
  const float block = logo + 34 * u + title_size + (subtitle.empty() ? 0 : 16 * u) +
                      subtitle.size() * subtitle_size * 1.3f + 96 * u + 90 * u;
  float y = h * 0.48f - block / 2;

  if (!s_logo_tried)
    LoadLogo();
  const float bob = static_cast<float>(std::sin(now * 2.4)) * 7 * u;
  if (s_logo)
  {
    dl->AddImage(*s_logo, {cx - logo / 2, y + bob}, {cx + logo / 2, y + logo + bob}, {0, 0}, {1, 1},
                 Fade(IM_COL32_WHITE, alpha));
  }
  y += logo + 34 * u;

  if (!config.title.empty())
  {
    const ImVec2 t = Measure(ExtraBold(), title_size, config.title);
    WriteOutlined(dl, ExtraBold(), title_size, {cx - t.x / 2, y}, Fade(IM_COL32_WHITE, alpha),
                  Fade(look.accent_deep, alpha), 6 * u, config.title);
  }
  y += title_size + 16 * u;
  for (const std::string& line : subtitle)
  {
    const ImVec2 t = Measure(Bold(), subtitle_size, line);
    Write(dl, Bold(), subtitle_size, {cx - t.x / 2, y}, Fade(look.label, alpha), line);
    y += subtitle_size * 1.3f;
  }
  y += 96 * u;

  // What's happening, and how far along when it's known.
  const std::size_t done = s_shaders_done, total = s_shaders_total;
  const float status_size = 28 * u;
  std::string status = config.starting;
  if (total > 0)
  {
    const int percent = static_cast<int>(done * 100 / total);
    status = config.shaders;
    const auto at = status.find("{n}");
    if (at != std::string::npos)
      status.replace(at, 3, std::to_string(percent));
  }
  const ImVec2 t = Measure(Bold(), status_size, status);
  Write(dl, Bold(), status_size, {cx - t.x / 2, y}, Fade(look.text, alpha), status);
  y += status_size + 24 * u;

  if (total > 0)
  {
    const float bar = 520 * u, thick = 18 * u;
    const ImVec2 p0 = {cx - bar / 2, y}, p1 = {cx + bar / 2, y + thick};
    dl->AddRectFilled(p0, p1, Fade(look.line, alpha), thick / 2);
    const float fill = std::max(bar * done / total, thick);
    const int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, {p0.x + fill, p1.y}, IM_COL32_WHITE, thick / 2);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, p0, {p0.x + bar, p0.y},
                                                  Fade(look.grad_top, alpha),
                                                  Fade(look.grad_bottom, alpha));
  }
  else
  {
    // Three dots, one after the other.
    const float r = 11 * u, gap = 40 * u;
    for (int i = 0; i < 3; ++i)
    {
      const double phase = now * 4.2 - i * 0.75;
      const float lift = static_cast<float>(std::max(0.0, std::sin(phase)));
      const ImVec2 c = {cx + (i - 1) * gap, y + r + 8 * u - lift * 10 * u};
      const ImU32 colour = i == 0 ? look.grad_bottom : i == 1 ? look.accent : look.accent_deep;
      dl->AddCircleFilled(c, r * (0.85f + 0.15f * lift), Fade(colour, alpha));
    }
  }
}

// How tall a notification is, `width` wide; its lines.
struct Layout
{
  std::vector<std::string> title;
  std::vector<std::string> text;
  // The texts and rows, and the whole card.
  float content = 0;
  float height = 0;
};

constexpr float WIDTH = 460, PAD = 22, ICON = 58, GAP = 18, TITLE = 27, BODY = 22, ROW = 46;

Layout Lay(const Toast& toast, float u)
{
  Layout layout;
  const float text_width = (WIDTH - 2 * PAD - ICON - GAP) * u;
  layout.title = Wrap(ExtraBold(), TITLE * u, toast.title, text_width);
  layout.text = Wrap(Bold(), BODY * u, toast.text, text_width);
  float h = layout.title.size() * TITLE * 1.25f * u + layout.text.size() * BODY * 1.35f * u;
  if (!layout.title.empty() && !layout.text.empty())
    h += 4 * u;
  if (!toast.rows.empty())
    h += 10 * u + toast.rows.size() * ROW * u;
  layout.content = h;
  layout.height = std::max(h, ICON * u) + 2 * PAD * u;
  return layout;
}

void DrawToast(ImDrawList* dl, const Toast& toast, const Layout& layout, ImVec2 pos, float u,
               float alpha)
{
  const Look& look = Get().look;
  const ImVec2 p1 = {pos.x + WIDTH * u, pos.y + layout.height};
  const float rounding = 30 * u;
  Shadow(dl, pos, p1, rounding, u, alpha);
  dl->AddRectFilled(pos, p1, Fade(look.card, alpha), rounding);
  dl->AddRect(pos, p1, Fade(toast.warning ? look.danger : look.accent_soft, alpha), rounding, 0, 4 * u);

  const ImVec2 icon = {pos.x + (PAD + ICON / 2) * u, pos.y + (PAD + ICON / 2) * u};
  dl->AddCircleFilled(icon, ICON / 2 * u, Fade(toast.warning ? look.danger_bg : look.tint, alpha));
  Icon(dl, toast.icon, icon, ICON * 0.56f * u, Fade(toast.warning ? look.danger : look.accent, alpha));

  const float x = pos.x + (PAD + ICON + GAP) * u;
  // A short one (a title alone) beside the middle of its icon.
  float y = pos.y + PAD * u + std::max(0.f, (ICON * u - layout.content) / 2);
  for (const std::string& line : layout.title)
  {
    Write(dl, ExtraBold(), TITLE * u, {x, y}, Fade(toast.warning ? look.danger : look.text, alpha), line);
    y += TITLE * 1.25f * u;
  }
  if (!layout.title.empty() && !layout.text.empty())
    y += 4 * u;
  for (const std::string& line : layout.text)
  {
    Write(dl, Bold(), BODY * u, {x, y}, Fade(look.muted, alpha), line);
    y += BODY * 1.35f * u;
  }
  if (!toast.rows.empty())
    y += 10 * u;
  const float right = p1.x - PAD * u;
  for (const Row& row : toast.rows)
  {
    // The player's badge, in their colour.
    const float badge_h = 32 * u;
    const ImVec2 b = Measure(ExtraBold(), 19 * u, row.badge);
    const float badge_w = std::max(b.x + 22 * u, 50 * u);
    const float mid = y + ROW * u / 2;
    dl->AddRectFilled({x, mid - badge_h / 2}, {x + badge_w, mid + badge_h / 2},
                      Fade(look.players[row.player], alpha), badge_h / 2);
    Write(dl, ExtraBold(), 19 * u, {x + (badge_w - b.x) / 2, mid - b.y / 2},
          Fade(IM_COL32_WHITE, alpha), row.badge);

    float end = right;
    if (row.battery)
    {
      const float gauge = 48 * u;
      Gauge(dl, {end - gauge, mid - gauge / 4}, gauge, *row.battery, u, alpha);
      end -= gauge + 10 * u;
    }
    const ImVec2 v = Measure(ExtraBold(), BODY * u, row.value);
    const bool low = row.battery && *row.battery < 15;
    Write(dl, ExtraBold(), BODY * u, {end - v.x, mid - v.y / 2},
          Fade(low ? look.danger : look.text, alpha), row.value);
    const ImVec2 l = Measure(Bold(), BODY * u, row.label);
    Write(dl, Bold(), BODY * u, {x + badge_w + 12 * u, mid - l.y / 2}, Fade(look.text, alpha), row.label);
    y += ROW * u;
  }
}

void DrawToasts(ImDrawList* dl, ImVec2 size, float u, double now, float dt)
{
  {
    std::lock_guard lock(s_incoming_mutex);
    while (!s_incoming.empty())
    {
      s_toasts.push_back(std::move(s_incoming.front()));
      s_incoming.pop_front();
    }
  }
  std::erase_if(s_toasts, [now](const Toast& t) { return t.shown_at >= 0 && now - t.shown_at > t.seconds; });

  const float margin = 32 * u;
  float stack = margin;
  std::size_t shown = 0;
  for (Toast& toast : s_toasts)
  {
    if (shown == SHOWN)
      break;
    ++shown;
    const bool arriving = toast.shown_at < 0;
    if (arriving)
    {
      toast.shown_at = now;
      toast.y = stack;
    }
    const double age = now - toast.shown_at;
    const float in = EaseOut(age / SLIDE);
    const float out = EaseOut((toast.seconds - age) / SLIDE);
    const float visible = std::min(in, out);
    toast.y += (stack - toast.y) * (1 - std::exp(-dt * 14));
    const Layout layout = Lay(toast, u);
    const ImVec2 pos = {size.x - margin - WIDTH * u + (1 - visible) * 70 * u, toast.y};
    DrawToast(dl, toast, layout, pos, u, visible);
    stack += (layout.height + 16 * u) * visible;
  }
}
}  // namespace

bool IsEnabled()
{
  return Get().enabled;
}

std::string Title()
{
  return Get().title;
}

std::string LogoPath()
{
  return Get().logo;
}

void AddFonts()
{
  if (!IsEnabled())
    return;
  static std::once_flag reading;
  std::call_once(reading, [] { std::thread(ReadStdin).detach(); });

  const std::string dir = File::GetSysDirectory() + DIR_SEP RESOURCES_DIR DIR_SEP "wp" DIR_SEP;
  const std::string bold = dir + "MPLUSRounded1c-Bold.ttf";
  const std::string extra_bold = dir + "MPLUSRounded1c-ExtraBold.ttf";
  if (!File::Exists(bold) || !File::Exists(extra_bold))
    return;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  // The first is everyone's: Dolphin's own texts are in the launcher's letters too.
  s_bold = io.Fonts->AddFontFromFileTTF(bold.c_str());
  s_extra_bold = io.Fonts->AddFontFromFileTTF(extra_bold.c_str());
}

void Shutdown()
{
  s_logo.reset();
  s_logo_tried = false;
  s_bold = nullptr;
  s_extra_bold = nullptr;
}

bool IsLoading()
{
  return IsEnabled() && !s_gone;
}

void GameFramePresented()
{
  double none = -1;
  s_first_frame.compare_exchange_strong(none, Now());
}

void GameShown()
{
  s_shown = true;
}

void SetShaderProgress(std::size_t done, std::size_t total)
{
  s_shaders_done = done;
  s_shaders_total = total;
}

void Draw()
{
  if (!IsEnabled())
    return;
  const double now = Now();
  const float dt = s_last_draw < 0 ? 0 : static_cast<float>(now - s_last_draw);
  s_last_draw = now;

  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float u = std::max(size.y / 1080, 0.6f);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddCallback(Linear, nullptr);

  if (s_loaded_at < 0)
  {
    const double first = s_first_frame;
    const std::string id = SConfig::GetInstance().GetGameID();
    const double wait = id == "SUPP01" || id == "SUPE01" ? WII_PARTY : OTHER_TITLE;
    if (s_shown || (first >= 0 && now - first > wait))
      s_loaded_at = now;
  }
  const float loading = s_loaded_at < 0 ? 1 : 1 - EaseOut((now - s_loaded_at) / FADE);
  if (loading > 0)
    DrawLoading(dl, size, u, loading, now);
  else
    s_gone = true;
  // The notifications wait for the game.
  if (s_loaded_at >= 0)
    DrawToasts(dl, size, u, now, dt);

  dl->AddCallback(Point, nullptr);
}

void Idle()
{
  if (!IsLoading() || !g_presenter)
    return;
  if (s_last_draw >= 0 && Now() - s_last_draw < IDLE_FRAME)
    return;
  g_presenter->Present();
}
}  // namespace WPOverlay
