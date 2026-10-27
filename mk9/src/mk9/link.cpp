// The game's side of the launcher protocol (docs/PROTOCOL.md section 3, MK9's keys): with
// MK9_LINK=1 the launcher owns this process. The game boots to the main menu and waits there (the
// hold point, "ready"), takes commands from cmd.txt (claimed by renaming it to cmd.txt.taken) and
// reports its state in status.txt (written to a .tmp file and moved over it).
//
//   start mode=versus|tag|tyl|practice slot=0 players=2 local_port=7001 peers=1@1.2.3.4:7002[,...]
//         delay=2 netcode=rollback|lockstep seed=N match=<id> [p1=..] [p2=..] [p3=..] [p4=..]
//         (remote=ip:port with side=1|2 is taken for a two-player match too)
//            the players meet at the hold point (netplay's first barrier), then the autopilot of
//            slot 0 takes both games through the menus to the mode's select screen, where each
//            player picks with their own controller (or the autopilot picks a given fighter)
//   park     back to the hold point
//   rematch  the next match with the same settings (new seed and id when given)
//   quit     exit
//
// status.txt: booting, ready, in_match match=<id>, result winner=N p1_rounds=.. p2_rounds=..
// match=<id>, desync frame=N match=<id>, disconnected match=<id>, error <text>.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk9/link.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include <rex/logging.h>

#include "mk9/autopilot.h"
#include "mk9/net/netplay.h"

namespace mk9::link {
namespace {

bool g_on = false;
std::wstring g_cmd, g_status;
std::string g_last_status;
std::string g_match = "none";
bool g_in_match = false;  // a start was taken and not parked
bool g_online = false;
bool g_reported = false;  // result / desync / disconnected written for this match
autopilot::Plan g_plan;
net::Settings g_net;

std::wstring Widen(const std::string& s) {
  std::wstring w;
  for (unsigned char c : s) w += wchar_t(c);
  return w;
}

void WriteStatus(const std::string& line) {
  if (!g_on || line == g_last_status) return;
  g_last_status = line;
  const std::wstring tmp = g_status + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  const std::string text = line + "\n";
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  for (int i = 0; i < 200; i++) {
    if (MoveFileExW(tmp.c_str(), g_status.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    Sleep(5);
  }
  REXLOG_INFO("mk9 link: status {}", line);
}

bool ClaimCommand(std::string& out) {
  const std::wstring taken = g_cmd + L".taken";
  if (!MoveFileExW(g_cmd.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
  FILE* f = _wfopen(taken.c_str(), L"rb");
  if (!f) return false;
  char buf[2048];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  out = buf;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return true;
}

struct Command {
  std::string verb;
  std::map<std::string, std::string> keys;
};

Command Parse(const std::string& line) {
  Command c;
  std::istringstream in(line);
  in >> c.verb;
  for (std::string tok; in >> tok;) {
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) c.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
  }
  return c;
}

std::string Key(const Command& c, const char* k, const char* def = "") {
  auto it = c.keys.find(k);
  return it == c.keys.end() ? def : it->second;
}

void Start(const Command& c) {
  if (!autopilot::AtHoldPoint()) {
    WriteStatus("error not at the hold point");
    return;
  }
  autopilot::Plan plan;
  plan.mode = autopilot::ModeFromName(Key(c, "mode", "versus"));
  if (plan.mode == autopilot::Mode::Unknown) {
    WriteStatus("error bad command mode " + Key(c, "mode"));
    return;
  }
  plan.players = std::atoi(Key(c, "players", "2").c_str());
  plan.seed = uint32_t(std::strtoul(Key(c, "seed", "0").c_str(), nullptr, 10));
  for (int i = 0; i < 4; i++) {
    const std::string k = "p" + std::to_string(i + 1);
    std::string pick = Key(c, k.c_str());
    if (pick == "-") pick.clear();
    plan.picks[i] = pick;
  }
  int slot = std::atoi(Key(c, "slot", "-1").c_str());
  if (slot < 0) slot = Key(c, "side", "1") == "2" ? 1 : 0;
  plan.slot = slot;
  g_match = Key(c, "match", plan.mode == autopilot::Mode::Practice ? "practice" : "none");

  // the network: peers=<slot>@ip:port,... or remote=ip:port (the other side of a two-player match)
  net::Settings st;
  st.slot = slot;
  st.players = plan.mode == autopilot::Mode::Practice ? 1 : std::max(2, plan.players);
  st.bind = uint16_t(std::atoi(Key(c, "local_port", "7001").c_str()));
  st.delay = std::atoi(Key(c, "delay", "-1").c_str());
  st.rollback = Key(c, "netcode", "lockstep") == "rollback";
  const std::string peers = Key(c, "peers"), remote = Key(c, "remote");
  if (!peers.empty()) {
    std::stringstream ss(peers);
    for (std::string item; std::getline(ss, item, ',');) {
      const size_t at = item.find('@');
      if (at != std::string::npos) st.peers.push_back({std::atoi(item.substr(0, at).c_str()), item.substr(at + 1)});
    }
  } else if (!remote.empty()) {
    st.peers.push_back({1 - slot, remote});
  }
  st.loopback_only = !st.peers.empty() && st.peers[0].address.rfind("127.", 0) == 0;
  if (const char* log = std::getenv("MK9_NET_LOG"); log && *log) {
    st.log = log;  // "{match}" becomes the match id, "{slot}" this player's slot
    if (size_t at = st.log.find("{match}"); at != std::string::npos) st.log.replace(at, 7, g_match);
    if (size_t at = st.log.find("{slot}"); at != std::string::npos) st.log.replace(at, 6, std::to_string(slot));
  }
  g_online = plan.mode != autopilot::Mode::Practice && !st.peers.empty();
  if (g_online) {
    net::Configure(st);
    net::RequestMeet();  // at the hold point, every player's game at its next frame
  } else {
    net::Disable();
  }
  g_net = st;
  g_plan = plan;
  g_in_match = true;
  g_reported = false;
  autopilot::Begin(plan, g_online);
  REXLOG_INFO("mk9 link: start {} slot {} of {}, match {}, {} {}", Key(c, "mode", "versus"), slot, st.players,
              g_match, g_online ? (st.rollback ? "rollback" : "lockstep") : "offline",
              g_online ? std::to_string(st.peers.size()) + " peers" : std::string());
}

void Park() {
  net::Disable();
  autopilot::Park();
  g_in_match = false;
  g_online = false;
  REXLOG_INFO("mk9 link: park");
}

void Rematch(const Command& c) {
  if (!g_in_match) {
    WriteStatus("error no match to repeat");
    return;
  }
  if (std::string m = Key(c, "match"); !m.empty()) g_match = m;
  if (std::string seed = Key(c, "seed"); !seed.empty()) g_plan.seed = uint32_t(std::strtoul(seed.c_str(), nullptr, 10));
  g_reported = false;
  autopilot::Rematch(g_plan);
  REXLOG_INFO("mk9 link: rematch, match {}", g_match);
}

}  // namespace

void Init() {
  const char* on = std::getenv("MK9_LINK");
  const char* cmd = std::getenv("MK9_CMD");
  const char* status = std::getenv("MK9_STATUS");
  if (!on || std::strcmp(on, "1") != 0 || !cmd || !*cmd || !status || !*status) return;
  g_on = true;
  g_cmd = Widen(cmd);
  g_status = Widen(status);
  WriteStatus("booting");
  autopilot::Init(true);
}

bool On() { return g_on; }

void OnFrame(uint64_t frame) {
  if (!g_on) return;
  autopilot::OnFrame(frame);
  if (frame % 6 == 0) {
    std::string line;
    if (ClaimCommand(line)) {
      REXLOG_INFO("mk9 link: command {}", line);
      const Command c = Parse(line);
      if (c.verb == "start") Start(c);
      else if (c.verb == "park") Park();
      else if (c.verb == "rematch") Rematch(c);
      else if (c.verb == "quit") {
        WriteStatus("quit");
        ExitProcess(0);
      } else {
        WriteStatus("error bad command");
      }
    }
  }
  // the state for the launcher
  if (!g_in_match) {
    WriteStatus(autopilot::AtHoldPoint() ? "ready" : "booting");
    return;
  }
  if (g_reported) return;
  if (g_online) {
    const net::Status s = net::GetStatus();
    if (s == net::Status::Desynced) {
      WriteStatus("desync frame=" + std::to_string(net::Frame()) + " match=" + g_match);
      g_reported = true;
      return;
    }
    if (s == net::Status::Disconnected) {
      WriteStatus("disconnected match=" + g_match);
      g_reported = true;
      return;
    }
  }
  WriteStatus("in_match match=" + g_match);
}

void ReportResult(int winner, int p1_rounds, int p2_rounds) {
  if (!g_on || !g_in_match || g_reported) return;
  WriteStatus("result winner=" + std::to_string(winner) + " p1_rounds=" + std::to_string(p1_rounds) +
              " p2_rounds=" + std::to_string(p2_rounds) + " match=" + g_match);
  g_reported = true;
}

void ReportProblem(const std::string& status) {
  if (!g_on || g_reported) return;
  WriteStatus(status + " match=" + g_match);
  g_reported = true;
}

}  // namespace mk9::link
