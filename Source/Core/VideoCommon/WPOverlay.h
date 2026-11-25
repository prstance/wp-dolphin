// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

// What the Wii Party launcher shows in the game's window. With WP_OVERLAY set to a JSON object (the
// launcher's colours, its logo, the title and texts of its loading screen):
//
// - a loading screen in the launcher's colours instead of a black window, until the game shows its
//   first scene (WPGameState), then it fades out;
// - notifications at the top right, one JSON object per line on stdin, shown once the loading
//   screen has gone:
//
//     {"icon": "remotes" | "battery" | "pointer" | "mods" | "save" | "mii" | "clock" | "info",
//      "tone": "normal" | "warning", "title": "…", "text": "…", "seconds": 6,
//      "rows": [{"player": 0, "badge": "J1", "label": "Wiimote", "value": "85 %", "battery": 85}]}
//
//   A row is a player's line: their colour (0 to 3), a gauge when "battery" is there.
//
// The texts come translated from the launcher; the fonts are its own (Sys/Resources/wp).
namespace WPOverlay
{
bool IsEnabled();

// The game window's title and icon, the launcher's ("" without them).
std::string Title();
std::string LogoPath();

// OnScreenUI::Initialize: the launcher's fonts, before ImGui's first frame. Dolphin's own texts
// (FPS, messages) are in them too.
void AddFonts();
// OnScreenUI's end: what was made for its backend.
void Shutdown();

// OnScreenUI::Finalize, on the video thread, with ImGui's lock held.
void Draw();
// The video thread, between two batches of the game's commands: keeps the loading screen moving
// while the game presents nothing (the disc booting, the game starting).
void Idle();

// The loading screen is up (the shaders' progress goes there rather than in Dolphin's window).
bool IsLoading();
// Presenter: one of the game's frames was presented.
void GameFramePresented();
// WPGameState: the game shows its first scene.
void GameShown();
// ShaderCache: the shaders compiled before the game starts (0, 0: done).
void SetShaderProgress(std::size_t done, std::size_t total);
}  // namespace WPOverlay
