// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class System;
}

// Where Wii Party is, for the launcher (its Discord Rich Presence). With WP_GAME_STATE set, the
// running scene, the game mode and the minigame go to stdout, one line each time they change:
//
//   wp-state <scene> <mode> <minigame>
//
// The scene is the name of the running REL ("menu", "mr001", "mg402"), or "-" between two.
//
// On the boards of Board Game Island, Globe Trot and Swap Meet, the players' numbers too:
//
//   wp-stats <mode> <round> <rounds> <player>…
//
// One word per player, "h" (human) or "c" (CPU) then its numbers, after ':':
//   Board Game Island (mode 0): spaces left to the final challenge, in the volcano, at the goal;
//                               no number of rounds (it's a race): 0
//   Globe Trot (mode 3):        coins, photos, turns left resting
//   Swap Meet (mode 4):         points
namespace WPGameState
{
// Called on the CPU thread for each field.
void Update(Core::System& system);
}  // namespace WPGameState
