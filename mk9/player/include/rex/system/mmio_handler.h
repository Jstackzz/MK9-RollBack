#pragma once
#include <cstdint>
namespace rex::runtime {
class MMIOHandler {
 public:
  static MMIOHandler* global_handler();
  bool CheckLoad(uint32_t virtual_address, uint32_t* out_value);
  bool CheckStore(uint32_t virtual_address, uint32_t value);
};
}  // namespace rex::runtime
