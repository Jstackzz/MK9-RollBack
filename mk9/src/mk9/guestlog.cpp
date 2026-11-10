// What a player's mk9game.dll asks mk9.exe to log (mk9/player/include/rex/logging.h): the translated code's only log
// lines, the guest's traps, built only for the package's ready-made mk9.exe.

#include <cstddef>
#include <string_view>

#include <fmt/format.h>

#include <rex/logging.h>

extern "C" __declspec(dllexport) void Mk9GuestLog(int level, const char* format, const char* text, size_t size,
                                                  unsigned value) {
  const std::string line = text ? fmt::format(fmt::runtime(format), std::string_view(text, size))
                                : fmt::format(fmt::runtime(format), value);
  if (level == 0) {
    REXCPU_DEBUG("{}", line);
  } else {
    REXCPU_WARN("{}", line);
  }
}
