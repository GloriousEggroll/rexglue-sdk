/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation Toolkit                                  *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::debug {

bool IsGuestFunctionTraceEnabled();
void TraceGuestFunction(uint32_t guest_address);
bool IsGuestFunctionStateTraceEnabled(uint32_t guest_address);
void TraceGuestFunctionState(uint32_t guest_address, const PPCContext& context,
                             const uint8_t* memory_base);
bool IsGuestIndirectCallTraceEnabled(uint32_t guest_lr);
void TraceGuestIndirectCall(uint32_t guest_lr, uint32_t guest_target,
                            const PPCContext& context);

}  // namespace rex::debug
