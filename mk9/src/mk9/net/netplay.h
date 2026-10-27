#pragma once
// Netplay for 2 to 4 players over UDP (each PC one player slot; slot = the game's controller
// user index, 0 = P1, 1 = P2, and 2-3 for a two-versus-two with four people). Each game runs on
// its own until a match is set up; all meet at a barrier (net::Meet, called by the game layer at
// the match's setup point), and from then on every slot's input for each frame is the same on
// every PC: the local player's own pad goes out to the others, theirs come in, and the game's
// pad reads (mk9/input.h) answer with the frame's inputs.
//
// Lockstep (version 1) waits for every other player's input for a frame before it runs. Rollback
// (version 2, mk9/net/rollback.h) runs ahead on predicted inputs and puts the game back when a
// prediction was wrong. Local input waits `delay` frames before it is used in both.
//
// Configuration, from the launcher link or, for tests, the environment:
//   MK9_NET_BIND=<udp port>              this game's port
//   MK9_NET_PEERS=<slot>@<ip>:<port>,... the other players (e.g. "1@127.0.0.1:7002")
//   MK9_NET_SLOT=<n>                     this player's slot
//   MK9_NET_PLAYERS=<n>                  slots in the match (2..4)
//   MK9_NET_DELAY=<frames>               input delay (default 2 lockstep, 1 rollback)
//   MK9_NET_MODE=lockstep|rollback
//   MK9_NET_LOG=<path>                   every frame's inputs and checks
//   MK9_NET_LAG=<ms>, _JITTER=<ms>, _LOSS=<percent>   simulated network on what this game sends
//   MK9_NET_FEED=<ip:port>               the launcher's spectator feed (TCP): a player's game writes
//                                        its barriers, inputs and checks there; with
//   MK9_NET_WATCH=1                      this game is a spectator: it reads every player from the
//                                        feed and plays the match without a player of its own
#include <cstdint>
#include <string>
#include <vector>

#include "mk9/input.h"

namespace mk9::net {

constexpr int kMaxSlots = 4;

struct Peer {
  int slot = -1;
  std::string address;  // ip:port
};

struct Settings {
  uint16_t bind = 0;
  int slot = 0;
  int players = 2;
  std::vector<Peer> peers;
  int delay = -1;  // -1: the mode's default
  bool rollback = false;
  bool loopback_only = false;
  std::string log;
  int lag_ms = 0, jitter_ms = 0, loss_percent = 0;
  std::string feed;    // ip:port of the launcher's spectator feed, or empty
  bool watch = false;  // a spectator (slot and peers unused: every player comes from the feed)
};

enum class Status { Off, Connecting, Barrier, InSync, Desynced, Disconnected };

// read MK9_NET_* and configure when they are there (tests); at start
void InitFromEnvironment();
// start or stop; applied by the main thread at the top of the next frame
void Configure(const Settings& st);
void Disable();
Status GetStatus();
bool InSync();
const char* StatusName(Status s);

// the frame numbers of the match in sync: the frame running now (0 at the barrier)
uint32_t Frame();

// The barrier: the game layer calls this on the main thread at the match's setup point with
// what it must agree on (words). It returns once every player arrived (true) or on timeout or a
// stop (false); `agreed` gets each slot's words and the seed side 0 proposed.
struct Agreement {
  uint32_t seed = 0;
  std::vector<uint32_t> words[kMaxSlots];
};
bool Meet(const std::vector<uint32_t>& words, uint32_t my_seed, Agreement& agreed);
// the launcher's start: meet the others at the top of a frame once connected (the hold point)
void RequestMeet();

// the frame's inputs for the game's pads: installed as mk9::input's source while in sync
bool FrameInput(uint32_t user, input::Pad& out);

// a check both PCs compare (the game layer's state hash at the top of a frame)
void Check(uint32_t frame, uint32_t hash);

struct Stats {
  uint32_t frames = 0, waits = 0, rollbacks = 0, max_rollback = 0, resent = 0;
  int ping_ms = 0;
};
Stats GetStats();

}  // namespace mk9::net
