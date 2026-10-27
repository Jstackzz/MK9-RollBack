// The sync test: rollback without a network. From frame MK9_SYNCTEST_FROM on (counted from boot),
// every MK9_SYNCTEST_EVERY frames the game is put back MK9_SYNCTEST (depth) frames and runs them
// again with the same inputs (a test script's, which follow the frame count); each frame run
// again must write the same pages with the same contents as the first time. Mismatches are
// logged with the pages that differ.
//   MK9_SYNCTEST=<depth>  MK9_SYNCTEST_FROM=<frame>  MK9_SYNCTEST_EVERY=<frames, default 10>

#include <chrono>
#include <cstdlib>
#include <map>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "mk9/frame.h"
#include "mk9/net/rollback.h"

namespace mk9::synctest {
namespace {

using Clock = std::chrono::steady_clock;

int g_depth = 0;
uint64_t g_from = 0;
int g_every = 10;
std::map<uint64_t, uint64_t> g_hash;  // frame -> its page hash the first time it ran
std::map<uint64_t, std::vector<uint32_t>> g_pages;
uint64_t g_checked = 0, g_bad = 0, g_rollbacks = 0;
double g_worst_ms = 0;

void OnTop(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = frame::Count();
  if (n < g_from) return;
  if (n == g_from) {
    rb::SetHashing(true);
    rb::Arm(ctx, uint32_t(n));
    return;
  }
  if (!rb::Armed()) return;
  rb::CloseFrame();
  g_hash[n - 1] = rb::LastFrameHash();
  g_pages[n - 1] = rb::LastFramePageList();
  while (!g_hash.empty() && g_hash.begin()->first + 64 < n) {
    g_hash.erase(g_hash.begin());
    g_pages.erase(g_pages.begin());
  }
  if ((n - g_from) % uint64_t(g_every) == 0 && n >= g_from + uint64_t(g_depth) + 1) {
    const PPCContext top = ctx;
    const uint64_t target = n - uint64_t(g_depth);
    const auto t0 = Clock::now();
    if (rb::Restore(uint32_t(target), ctx)) {
      g_rollbacks++;
      frame::SetCount(target);
      for (uint64_t f = target; f < n; f++) {
        ctx = top;
        rb::OpenFrame(uint32_t(f), ctx);
        frame::RunAgain(ctx, base);
        rb::CloseFrame();
        const uint64_t again = rb::LastFrameHash();
        g_checked++;
        auto it = g_hash.find(f);
        if (it != g_hash.end() && it->second != again) {
          g_bad++;
          if (g_bad <= 20) {
            auto now_pages = rb::LastFramePageList();
            REXLOG_ERROR("mk9 synctest: frame {} ran again differently ({} pages first, {} again)", f,
                         g_pages[f].size(), now_pages.size());
          }
        }
        g_hash[f] = again;
        g_pages[f] = rb::LastFramePageList();
      }
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      g_worst_ms = std::max(g_worst_ms, ms);
    }
    ctx = top;
  }
  rb::OpenFrame(uint32_t(n), ctx);
  if (n % 600 == 0) {
    auto st = rb::GetStats();
    REXLOG_INFO(
        "mk9 synctest: frame {}: {} rollbacks, {} frames checked, {} differed; last frame {} pages, {} foreign "
        "pages, close {:.2f} ms, restore {:.2f} ms, worst rollback {:.1f} ms",
        n, g_rollbacks, g_checked, g_bad, st.last_pages, st.foreign_pages, st.close_ms, st.restore_ms, g_worst_ms);
  }
}

}  // namespace

bool Init() {
  const char* d = std::getenv("MK9_SYNCTEST");
  if (!d || !*d) return false;
  g_depth = std::max(1, std::atoi(d));
  if (const char* v = std::getenv("MK9_SYNCTEST_FROM")) g_from = std::strtoull(v, nullptr, 10);
  if (const char* v = std::getenv("MK9_SYNCTEST_EVERY")) g_every = std::max(1, std::atoi(v));
  frame::SetTopHook(&OnTop);
  REXLOG_INFO("mk9 synctest: depth {} every {} frames from frame {}", g_depth, g_every, g_from);
  return true;
}

}  // namespace mk9::synctest
