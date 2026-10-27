#pragma once
// The random number trace for finding where two games' random streams part (MK9_TRACE_RAND).
#include <cstdint>

namespace mk9::randtrace {

// one line of the trace (when tracing): kind, the guest caller, the value
void Log(char kind, uint32_t caller, uint32_t value);

}  // namespace mk9::randtrace
