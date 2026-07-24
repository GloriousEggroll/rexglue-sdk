/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation Toolkit                                  *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

namespace rex::debug {

bool IsGuestFunctionTraceEnabled();
void TraceGuestFunction(uint32_t guest_address);

}  // namespace rex::debug
