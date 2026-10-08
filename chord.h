// Chorded activation: an input that needs a modifier held while a key or button is pressed.
// Header-only, so tests\chordtest.cpp can check the timing rules without the game.
#pragma once
#include <cstdint>

namespace chord {

struct State { bool keyPrev = false, modPrev = false, on = false, used = false; uint32_t keyAt = 0; };

const uint32_t kGraceMs = 150;      // the key may land this long before the modifier and still count

// One poll: is the modifier held, is the key held, the time in milliseconds. Returns whether the chord is
// held. It starts when the key goes down with the modifier held, or when the modifier follows the key
// within kGraceMs (both pressed "at once"), and lasts until either is released. One press of the key makes
// one chord at most, and a key that has been held for longer does not start a chord when the modifier
// arrives: on its own it stays the game's input.
inline bool down(State& s, bool mod, bool key, uint32_t now) {
    if (key && !s.keyPrev) { s.keyAt = now; s.used = false; }
    if (key && mod && !s.on && !s.used && (!s.keyPrev || (!s.modPrev && now - s.keyAt <= kGraceMs))) s.on = s.used = true;
    if (!key || !mod) s.on = false;
    s.keyPrev = key; s.modPrev = mod;
    return s.on;
}

} // namespace chord
