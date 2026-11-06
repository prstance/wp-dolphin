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
namespace WPGameState
{
// Called on the CPU thread for each field.
void Update(Core::System& system);
}  // namespace WPGameState
