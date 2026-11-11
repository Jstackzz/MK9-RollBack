// F3 reports, see report.h. The pictures are taken and written on threads of their own: the game does not wait for them.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk9/report.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/logging.h>

#include "mk9/game.h"
#include "mk9/names.h"
#include "mk9/net/netplay.h"
#include "mk9/snap.h"

#ifndef MK9_VERSION
#define MK9_VERSION "0.0.0"
#endif

namespace mk9::report {
namespace {

namespace fs = std::filesystem;

constexpr int kNextFrames = 20;              // the pictures after the one F3 caught: what only flickers shows in them
constexpr uint32_t kFrameWidth = 1920, kNextWidth = 1280;  // at most, as saved (a game drawn at 4K would be 25 MB each)
constexpr int kMaxReports = 5;               // a game's: each is tens of MB until the launcher has sent it
constexpr size_t kMaxWaiting = 6;            // pictures waiting to be written: past that a frame goes without one

fs::path g_dir;
std::set<uint64_t> g_test;
std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_saving{false};
bool g_down = false;  // main thread
int g_reports = 0;    // main thread

bool InFront() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

// what the game was doing, read on the main thread
std::string State(uint64_t frame) {
  std::string s = fmt::format("game {}\nframes run: {}\n", MK9_VERSION, frame);
  const net::Status st = net::GetStatus();
  if (st != net::Status::Off) {
    const net::Stats n = net::GetStats();
    s += fmt::format("netplay: {}, frame {}, ping {} ms, {} rollbacks (longest {} frames), {} waits\n",
                     net::StatusName(st), net::Frame(), n.ping_ms, n.rollbacks, n.max_rollback, n.waits);
  }
  s += fmt::format("game: phase {}, screen 0x{:X}, round {}, timer {}\n", game::Phase(), game::UiScreen(), game::Round(),
                   game::RoundTimer());
  return s;
}

struct Picture {
  snap::Image image;
  std::string name;
  uint32_t width = 0;
};

// One thread takes each frame's picture as soon as there is one, another writes them, so that a slow disk costs a
// picture and not the frames after it. state.txt comes last: it tells the launcher the report is whole.
void Save(fs::path folder, std::string state) {
  std::mutex lock;
  std::condition_variable more;
  std::deque<Picture> waiting;
  bool done = false;
  std::thread writer([&] {
    for (;;) {
      Picture p;
      {
        std::unique_lock<std::mutex> hold(lock);
        more.wait(hold, [&] { return !waiting.empty() || done; });
        if (waiting.empty()) return;
        p = std::move(waiting.front());
        waiting.pop_front();
      }
      if (!snap::Save(folder / p.name, p.image, p.width)) REXLOG_WARN("mk9 report: {} could not be written", p.name);
    }
  });
  const auto take = [&](const std::string& name, uint32_t width) {
    {
      std::lock_guard<std::mutex> hold(lock);
      if (waiting.size() >= kMaxWaiting) return;
    }
    Picture p;
    p.name = name;
    p.width = width;
    if (!snap::CaptureNow(p.image)) return;
    {
      std::lock_guard<std::mutex> hold(lock);
      waiting.push_back(std::move(p));
    }
    more.notify_one();
  };
  take("final.png", kFrameWidth);
  uint64_t last = g_frame.load(std::memory_order_relaxed);
  for (int i = 0; i < kNextFrames; i++) {
    // the next frame; a game that has stopped (paused, closing) does not hold the report back for long
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_frame.load(std::memory_order_relaxed) == last && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    last = g_frame.load(std::memory_order_relaxed);
    char name[32];
    std::snprintf(name, sizeof name, "next_%02d.png", i);
    take(name, kNextWidth);
  }
  {
    std::lock_guard<std::mutex> hold(lock);
    done = true;
  }
  more.notify_one();
  writer.join();
  if (FILE* f = _wfopen((folder / "state.txt").wstring().c_str(), L"wb")) {
    std::fwrite(state.data(), 1, state.size(), f);
    std::fclose(f);
  }
  REXLOG_INFO("mk9 report: saved");
  g_saving.store(false);
}

}  // namespace

void Init() {
  if (const char* d = std::getenv("MK9_CAPTURE_DIR"); d && *d) g_dir = d;
  if (const char* v = std::getenv("MK9_TEST_F3_AT"); v && *v) {
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) g_test.insert(std::strtoull(item.c_str(), nullptr, 10));
  }
}

void OnFrame(uint64_t frame) {
  if (g_dir.empty()) return;
  g_frame.store(frame, std::memory_order_relaxed);
  const bool down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
  const bool pressed = (down && !g_down && InFront()) || g_test.count(frame) != 0;
  g_down = down;
  if (!pressed || g_saving.load()) return;
  if (g_reports >= kMaxReports) {
    names::ShowNotice("F3: this game has saved as many reports as it can");
    return;
  }
  g_reports++;
  g_saving.store(true);
  SYSTEMTIME now;
  GetLocalTime(&now);
  char name[32];
  std::snprintf(name, sizeof name, "%02u%02u%02u", now.wHour, now.wMinute, now.wSecond);
  const fs::path folder = g_dir / name;
  std::error_code ec;
  fs::create_directories(folder, ec);
  REXLOG_INFO("mk9 report: F3 at frame {}, saving to {}", frame, name);
  names::ShowNotice("F3: what is on screen was saved; the launcher sends it to the developers with your log");
  std::thread(Save, folder, State(frame)).detach();
}

}  // namespace mk9::report
