// Checks the recompiler puts into the game's code ([[midasm_hook]] in mk9/config/mk9_codegen.toml):
// places where the game's own code went on through a pointer that can be empty here, and crashed.
// Each takes the function's own way out instead, and says so in the log (the first time and every
// hundredth), which a crash report quotes.

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

std::atomic<uint32_t> g_spawn{0}, g_effect{0}, g_render{0};

bool Guard(const PPCRegister& reg, std::atomic<uint32_t>& count, const char* what) {
  if (reg.u32 != 0) return false;
  const uint32_t n = ++count;
  if (n == 1 || n % 100 == 0) REXLOG_WARN("mk9 guard: {} ({} times)", what, n);
  return true;
}

}  // namespace

// sub_82A30C20, the script command that spawns an effect, wrote to what the spawn returned even when
// that was nothing (every effect in use: mk9/fx.h): its "return -1" instead
bool Mk9SpawnFailed(PPCRegister& r3) {
  return Guard(r3, g_spawn, "a script's effect could not be spawned, every effect in use: left out");
}

// sub_828E4720 read the new effect's id from nothing: it returns the nothing it got
bool Mk9EffectMissing(PPCRegister& r3) {
  return Guard(r3, g_effect, "an effect could not be spawned, every effect in use: left out");
}

// sub_82A569E0, run by a rendering command (sub_82A57390), sets three values on an entry of a render
// object and wrote through an entry that was not there (a Test Your Luck fight's first seconds):
// nothing to set then
bool Mk9RenderEntryMissing(PPCRegister& r11) {
  return Guard(r11, g_render, "a rendering command's entry was not there yet: skipped");
}
