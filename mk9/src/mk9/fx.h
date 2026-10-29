#pragma once
// The game's pool of effects (sparks, blood, fire, an arena's ambience: MK's effect manager).
//
// sub_82608E78 and sub_82608FC8 spawn an effect from a template; sub_82606510 takes the effect it
// uses off the manager's free list (a UE3 array at +664) and returns nothing once that is empty;
// sub_82606650 releases one, back onto the free list when it can be used again and the list holds
// fewer than +676 (64). The pool holds 64 effects. Once every one was in use, the game's own code
// wrote through the nothing a spawn returned (sub_82A30C20, a script command, among others): the
// crash of a screen full of effects. Here the oldest effect in use makes room instead (fx.cpp).
//
// MK9_FX_TRACE=1 logs how many effects are free as the game takes them.

#include <cstdint>

namespace mk9::fx {

struct Stats {
  uint64_t taken = 0, released = 0, returned = 0, reclaimed = 0, empty = 0;
};
Stats GetStats();

}  // namespace mk9::fx
