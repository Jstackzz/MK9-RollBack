#pragma once
// The random number trace for finding where two games' random streams part (MK9_TRACE_RAND).
#include <cstdint>

namespace mk9::randtrace {

// one line of the trace (when tracing): kind, the guest caller, the value, and the caller's own
// return address when known (draws through a helper)
void Log(char kind, uint32_t caller, uint32_t value, uint32_t up = 0);

}  // namespace mk9::randtrace
