#pragma once
// The audio device's pool of audio components (UFmodAudioComponent, sound.cpp), one for each sound
// playing. Called the game's effects here since 0.5.0, for the script command that takes them: what
// it spawns are sounds (found 2026-11-04: the "manager" is UFmodAudioDevice, the "template" a
// UFmodEvent).
//
// sub_82608E78 and sub_82608FC8 play a sound of an event; sub_82606510 takes the component it uses
// off the device's free list (a UE3 array at +664) and returns nothing once that is empty;
// sub_82606650 releases one, back onto the free list when it can be used again and the list holds
// fewer than +676 (64). The pool holds 64. Once every one was in use, the game's own code wrote
// through the nothing a spawn returned (sub_82A30C20, the script's sound command, among others):
// the crash of a screen full of sounds. Here the oldest in use makes room instead (fx.cpp).
//
// MK9_FX_TRACE=1 logs how many are free as the game takes them.

#include <cstdint>

namespace mk9::fx {

struct Stats {
  uint64_t taken = 0, released = 0, returned = 0, reclaimed = 0, empty = 0;
};
Stats GetStats();

}  // namespace mk9::fx
