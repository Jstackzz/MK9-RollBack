// The translated code logs only guest traps (ppc_trap in the generated header); mk9.exe writes those.
#pragma once
#include <cstddef>
#include <string_view>
extern "C" void Mk9GuestLog(int level, const char* format, const char* text, size_t size, unsigned value);
inline void Mk9GuestLogArg(int level, const char* format, std::string_view text) {
  Mk9GuestLog(level, format, text.data(), text.size(), 0);
}
inline void Mk9GuestLogArg(int level, const char* format, unsigned value) {
  Mk9GuestLog(level, format, nullptr, 0, value);
}
#define REXCPU_DEBUG(format, argument) Mk9GuestLogArg(0, format, argument)
#define REXCPU_WARN(format, argument) Mk9GuestLogArg(1, format, argument)
