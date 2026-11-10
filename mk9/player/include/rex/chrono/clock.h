#pragma once
#include <cstdint>
namespace rex::chrono {
class Clock {
 public:
  static uint64_t QueryGuestTickCount();
};
}  // namespace rex::chrono
