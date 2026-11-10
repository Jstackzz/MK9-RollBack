#pragma once
#include <mutex>
namespace rex::thread {
class global_critical_region {
 public:
  static std::recursive_mutex& mutex();
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }
};
}  // namespace rex::thread
