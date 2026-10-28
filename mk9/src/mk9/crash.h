#pragma once
// Crash reports. A game that crashes writes what happened, where, and what it was doing to
// crash.txt, and a minidump for a debugger to crash.dmp, in its session folder (MK9_SESSION_DIR,
// which the launcher gives every game it starts; crashes\ beside the exe otherwise), then ends at
// once with the crash's code as its exit code. The launcher builds the report the player sees
// from those, the exit code and the end of the game's log.
//
// Covered: exceptions nothing handled, on any thread (access violations, stack overflows, illegal
// instructions...), abort(), std::terminate, pure virtual calls and the C runtime's invalid
// parameter checks. Not covered: what ends the process without running any code of its own
// (__fastfail, a kill); the launcher reports those from the exit code and the log alone.
//
// Where in the game: a host address in mk9.exe that starts a recompiled function is named after
// its guest function (sub_82xxxxxx); the crashed thread's guest registers and guest call stack
// come with it when it is a guest thread.
//
// MK9_CRASH_TEST=<frame>[:abort|:overflow|:throw] crashes the game on purpose at the top of that
// frame (tests): an access violation, abort(), a stack overflow or a C++ exception nothing catches.

#include <cstdint>

namespace mk9::crash {

// installs the handlers; a static initializer does it at start, the app again once the runtime is
// up (a library may have put its own in meanwhile)
void Install();

// what the game is doing, one short line each, for a report: kept in fixed buffers that a crash
// can read whatever state the rest of the process is in
enum class Note { Status, Match, Count };
void SetNote(Note which, const char* text);

// the top of every frame (tests: MK9_CRASH_TEST)
void OnFrame(uint64_t frame);

}  // namespace mk9::crash
