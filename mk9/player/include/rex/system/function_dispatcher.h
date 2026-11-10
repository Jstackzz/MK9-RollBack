// (for mk9_register.cpp, which build.ninja leaves out: one game is not a set of guest DLLs)
#pragma once
#include <cstdint>
#include <rex/ppc/func.h>
namespace rex::runtime {
class IModuleRegistrar {
 public:
  virtual bool SetFunction(uint32_t guest_address, ::PPCFunc* func) = 0;

 protected:
  ~IModuleRegistrar() = default;
};
}  // namespace rex::runtime
