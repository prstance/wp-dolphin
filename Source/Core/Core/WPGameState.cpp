// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WPGameState.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "VideoCommon/WPOverlay.h"

namespace WPGameState
{
// In the main.dol of SUPP01 and SUPE01, which is the same file: SceneManager's running scene (a
// Scene*, with its name at +4), and the save block's game mode and current minigame.
constexpr u32 CURRENT_SCENE = 0x802F5D9C;
constexpr u32 GAME_MODE = 0x80231D6C;
constexpr u32 MINIGAME = 0x80235D8C;
// Twice a second.
constexpr int FIELDS = 30;
// The save block's number of players, its table of players (0x30 each, the CPU flag at +0xC) and
// Swap Meet's number of rounds.
constexpr u32 PLAYER_COUNT = 0x80231D74;
constexpr u32 PLAYERS = 0x80231980;
constexpr u32 SWAP_MEET_ROUNDS = 0x80231D77;

constexpr s32 BOARD_GAME_ISLAND = 0;
constexpr s32 GLOBE_TROT = 3;
constexpr s32 SWAP_MEET = 4;

// A pointer the game could have made: in MEM1 or MEM2.
static std::optional<u32> ReadPointer(const Core::CPUThreadGuard& guard, u32 address)
{
  const auto value = PowerPC::MMU::HostTryRead<u32>(guard, address);
  if (!value)
    return std::nullopt;
  const u32 p = value->value;
  if ((p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000))
    return p;
  return std::nullopt;
}

static std::optional<s32> ReadS32(const Core::CPUThreadGuard& guard, u32 address)
{
  const auto value = PowerPC::MMU::HostTryRead<u32>(guard, address);
  return value ? std::optional(s32(value->value)) : std::nullopt;
}

static std::optional<u8> ReadU8(const Core::CPUThreadGuard& guard, u32 address)
{
  const auto value = PowerPC::MMU::HostTryRead<u8>(guard, address);
  return value ? std::optional(value->value) : std::nullopt;
}

// The running REL's .bss: Scene+0 is its RelModule, whose mBss is at +0x10.
static std::optional<u32> ReadBss(const Core::CPUThreadGuard& guard)
{
  const auto scene = ReadPointer(guard, CURRENT_SCENE);
  const auto module = scene ? ReadPointer(guard, *scene) : std::nullopt;
  return module ? ReadPointer(guard, *module + 0x10) : std::nullopt;
}

// Numbers read in the middle of a change can be anything: only those the game can show.
static bool Plausible(s32 value, s32 max)
{
  return value >= 0 && value <= max;
}

// Board Game Island (mr001): GameState::sInstance at .bss+0x24, the same in the disc's REL and in
// the one the launcher links with the mods (theirs come after the core's).
static std::optional<std::string> IslandStats(const Core::CPUThreadGuard& guard, u32 bss, int players,
                                              std::vector<std::string>& words)
{
  const auto state = ReadPointer(guard, bss + 0x24);
  const auto round = state ? ReadS32(guard, *state + 0x34) : std::nullopt;
  const auto island = state ? ReadPointer(guard, *state + 0x1C) : std::nullopt;
  const auto manager = island ? ReadPointer(guard, *island + 0x98) : std::nullopt;
  const auto list = manager ? ReadPointer(guard, *manager + 0x1C0) : std::nullopt;
  if (!round || !Plausible(*round, 99) || !list)
    return std::nullopt;
  for (int i = 0; i < players; ++i)
  {
    const auto left = ReadS32(guard, *manager + 0x1F4 + 4 * i);
    const auto player = ReadPointer(guard, *list + 4 * i);
    const auto space = player ? ReadPointer(guard, *player + 0x2E8) : std::nullopt;
    const auto goal = player ? ReadU8(guard, *player + 0x326) : std::nullopt;
    if (!left || !Plausible(*left, 999) || !space || !goal)
      return std::nullopt;
    // In the volcano: the space has SPACE_FLAG_CAVE among its flags (a vector, not a mask).
    bool cave = false;
    const auto flags = ReadPointer(guard, *space + 0xB8);
    const auto count = ReadS32(guard, *space + 0xBC);
    if (flags && count && Plausible(*count, 32))
    {
      for (s32 f = 0; f < *count && !cave; ++f)
        cave = ReadS32(guard, *flags + 4 * f) == 0x20000;
    }
    words[i] += fmt::format(":{}:{}:{}", *left, int(cave), int(*goal != 0));
  }
  return fmt::format("{} 0", *round);
}

// Globe Trot (mr031): sMr031Status at .bss+0x11C, its players (0x214 each) at +0x14.
static std::optional<std::string> GlobeTrotStats(const Core::CPUThreadGuard& guard, u32 bss,
                                                 int players, std::vector<std::string>& words)
{
  const auto status = ReadPointer(guard, bss + 0x11C);
  const auto list = status ? ReadPointer(guard, *status + 0x14) : std::nullopt;
  const auto turn = status ? ReadS32(guard, *status + 0x24) : std::nullopt;
  const auto rounds = status ? ReadU8(guard, *status + 0x29) : std::nullopt;
  if (!list || !turn || !Plausible(*turn, 999) || !rounds)
    return std::nullopt;
  for (int i = 0; i < players; ++i)
  {
    const u32 player = *list + 0x214 * i;
    const auto coins = ReadS32(guard, player + 0x2C);
    const auto photos = ReadU8(guard, player + 0x4C);
    const auto resting = ReadS32(guard, player + 0x38);
    if (!coins || !Plausible(*coins, 999999) || !photos || !resting || !Plausible(*resting, 99))
      return std::nullopt;
    words[i] += fmt::format(":{}:{}:{}", *coins, *photos, *resting);
  }
  return fmt::format("{} {}", *turn / players + 1, *rounds);
}

// Swap Meet (mr041): its GameState at .bss+0x1C, the scores at +0x6C.
static std::optional<std::string> SwapMeetStats(const Core::CPUThreadGuard& guard, u32 bss,
                                                int players, std::vector<std::string>& words)
{
  const auto state = ReadPointer(guard, bss + 0x1C);
  const auto round = state ? ReadS32(guard, *state + 0x10) : std::nullopt;
  const auto set = ReadU8(guard, SWAP_MEET_ROUNDS);
  if (!round || !Plausible(*round, 99) || !set)
    return std::nullopt;
  // As Mr041Proc reads it: 0 is the usual 3, out of range 1.
  const s8 n = s8(*set);
  const int rounds = n == 0 ? 3 : (n < 0 || n > 9) ? 1 : n;
  for (int i = 0; i < players; ++i)
  {
    const auto points = ReadS32(guard, *state + 0x6C + 4 * i);
    if (!points || *points < -999 || *points > 9999)
      return std::nullopt;
    words[i] += fmt::format(":{}", *points);
  }
  return fmt::format("{} {}", std::min(*round + 1, rounds), rounds);
}

// The players' numbers on a party board, or nothing elsewhere.
static std::string ReadStats(const Core::CPUThreadGuard& guard, const std::string& scene, s32 mode)
{
  const bool board = (mode == BOARD_GAME_ISLAND && scene == "mr001") ||
                     (mode == GLOBE_TROT && scene == "mr031") ||
                     (mode == SWAP_MEET && scene == "mr041");
  const auto bss = board ? ReadBss(guard) : std::nullopt;
  const auto count = ReadU8(guard, PLAYER_COUNT);
  if (!bss || !count || *count < 1 || *count > 4)
    return {};
  const int players = *count;
  std::vector<std::string> words;
  for (int i = 0; i < players; ++i)
    words.push_back(ReadU8(guard, PLAYERS + 0x30 * i + 0xC).value_or(0) ? "c" : "h");

  std::optional<std::string> rounds;
  if (mode == BOARD_GAME_ISLAND)
    rounds = IslandStats(guard, *bss, players, words);
  else if (mode == GLOBE_TROT)
    rounds = GlobeTrotStats(guard, *bss, players, words);
  else
    rounds = SwapMeetStats(guard, *bss, players, words);
  if (!rounds)
    return {};
  std::string line = fmt::format("wp-stats {} {}", mode, *rounds);
  for (const std::string& word : words)
    line += ' ' + word;
  return line;
}

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
  static std::string last_stats;
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
  const std::string scene = ReadScene(guard);
  if (scene != "-")
    WPOverlay::GameShown();
  const std::string line =
      fmt::format("wp-state {} {} {}", scene, s32(mode->value), s32(minigame->value));
  // Off the boards, the last numbers still count: the launcher keeps them for when it's back.
  const std::string stats = ReadStats(guard, scene, s32(mode->value));
  const auto print = [](const std::string& text, std::string& before) {
    if (text.empty() || text == before)
      return;
    before = text;
    std::fputs((text + '\n').c_str(), stdout);
  };
  print(line, last);
  print(stats, last_stats);
  std::fflush(stdout);
}
}  // namespace WPGameState
