// MK9_TRACE_RAND=<path>: every draw from MK9's random number generators, one line each:
//   <netplay frame> <boot frame> <kind> <caller> <value> <thread>
// kind G: the game's generator gave an int (sub_826D91C0), F: a float (sub_826D91E8), S: it was
// seeded (sub_826D3088), with the generator's state after (those hooks are game.cpp's); C: the C
// runtime's rand() (sub_82B2CFB0, which only FMOD's sounds use), with its result. caller is the
// guest return address, thread m (the main thread) or o (any other). Two games' traces side by
// side show where their random streams part (tools/randcmp.py).

#include "mk9/randtrace.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "mk9/frame.h"
#include "mk9/net/netplay.h"

REX_EXTERN(__imp__sub_82B2CFB0);

namespace mk9::randtrace {
namespace {

FILE* Trace() {
  static FILE* f = [] {
    const char* p = std::getenv("MK9_TRACE_RAND");
    return p && *p ? std::fopen(p, "w") : nullptr;
  }();
  return f;
}

std::mutex g_lock;

}  // namespace

void Log(char kind, uint32_t caller, uint32_t value) {
  FILE* f = Trace();
  if (!f) return;
  const bool main = frame::OnMainThread();
  std::lock_guard lock(g_lock);
  std::fprintf(f, "%u %u %c %08X %08X %c\n", unsigned(net::InSync() ? net::Frame() : 0),
               unsigned(frame::Count()), kind, caller, value, main ? 'm' : 'o');
  std::fflush(f);  // the games are killed, not closed, when a test ends
}

}  // namespace mk9::randtrace

REX_HOOK_RAW(sub_82B2CFB0) {
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82B2CFB0(ctx, base);
  if (mk9::frame::OnMainThread()) mk9::randtrace::Log('C', caller, ctx.r3.u32);
}
