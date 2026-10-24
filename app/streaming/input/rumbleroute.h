// SPDX-License-Identifier: GPL-3.0-or-later
// Which gamepads a host rumble request reaches. The host addresses the player number; several
// local devices can report the same player (single-controller mode: the Deck's pad and a
// DualSense both report player 0, and a Steam Controller (2026) link can share player 0 with an
// SDL pad). Pure so the decision is testable without SDL.
#pragma once

#include <vector>

namespace RumbleRoute {

struct Pad {
    int slot;      // SDL gamepad slot
    int player;    // the player number the slot reports to the host
    bool ps5;      // SDL_CONTROLLER_TYPE_PS5
};

// The SDL slots that take a rumble for `player`. Among the pads mapped to the player, PS5 pads
// win (indexing by slot would rumble the Deck instead of the DualSense); otherwise every mapped
// pad rumbles. A Steam Controller link that took the request does not remove the SDL pads that
// share its player number: the fan-out is the same with or without it.
inline std::vector<int> sdlSlots(const std::vector<Pad>& pads, int player)
{
    bool preferPs5 = false;
    for (const Pad& pad : pads) {
        if (pad.player == player && pad.ps5) { preferPs5 = true; break; }
    }
    std::vector<int> matched;   // not 'slots': that is a Qt keyword macro in the app build
    for (const Pad& pad : pads) {
        if (pad.player != player) continue;
        if (preferPs5 && !pad.ps5) continue;
        matched.push_back(pad.slot);
    }
    return matched;
}

// Whether anything took the request: a Steam link, an SDL pad, or both.
inline bool routed(bool steamTook, const std::vector<int>& sdlSlotsTaken)
{
    return steamTook || !sdlSlotsTaken.empty();
}

}  // namespace RumbleRoute
