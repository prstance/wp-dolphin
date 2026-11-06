// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WPGameState.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace WPGameState
{
// In the main.dol of SUPP01 and SUPE01, which is the same file: SceneManager's running scene (a
// Scene*, with its name at +4), and the save block's game mode and current minigame.
constexpr u32 CURRENT_SCENE = 0x802F5D9C;
constexpr u32 GAME_MODE = 0x80231D6C;
constexpr u32 MINIGAME = 0x80235D8C;
// Twice a second.
constexpr int FIELDS = 30;

// MSL's std::string: a short one holds its size in the low 7 bits of its first byte, and its
// characters right after; a long one sets the top bit. Scene names are always short.
static std::string ReadScene(const Core::CPUThreadGuard& guard)
{
  const auto scene = PowerPC::MMU::HostTryRead<u32>(guard, CURRENT_SCENE);
  if (!scene || scene->value == 0)
    return "-";
  const auto head = PowerPC::MMU::HostTryRead<u8>(guard, scene->value + 4);
  if (!head || (head->value & 0x80) != 0 || (head->value & 0x7F) == 0)
    return "-";
  const auto name = PowerPC::MMU::HostTryReadString(guard, scene->value + 5, head->value & 0x7F);
  if (!name || !std::ranges::all_of(name->value, [](char c) { return std::isalnum(u8(c)); }))
    return "-";
  return name->value;
}

void Update(Core::System& system)
{
  static const bool enabled = std::getenv("WP_GAME_STATE") != nullptr;
  static int countdown = 0;
  static std::string last;
  if (!enabled || --countdown > 0)
    return;
  countdown = FIELDS;

  const std::string id = SConfig::GetInstance().GetGameID();
  if (id != "SUPP01" && id != "SUPE01")
    return;

  const Core::CPUThreadGuard guard(system);
  const auto mode = PowerPC::MMU::HostTryRead<u32>(guard, GAME_MODE);
  const auto minigame = PowerPC::MMU::HostTryRead<u32>(guard, MINIGAME);
  if (!mode || !minigame)
    return;
  const std::string line = fmt::format("wp-state {} {} {}", ReadScene(guard), s32(mode->value),
                                       s32(minigame->value));
  if (line == last)
    return;
  last = line;
  std::fputs((line + '\n').c_str(), stdout);
  std::fflush(stdout);
}
}  // namespace WPGameState
