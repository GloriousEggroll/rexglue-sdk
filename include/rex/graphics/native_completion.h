#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace rex::graphics {

enum class NativeCompletionReason {
  CommandPacket,
  ScratchWrite,
  Wait,
  PrimaryBufferEnd,
  Presentation,
  ReferenceDraw,
  ProviderRemoval,
  QueryPressure,
  Shutdown,
};

// A native-owned boundary does not need a fresh reference command buffer.
// Opening the guest frame still performs frame-resource reclamation, and
// handback or a suspended reference query must retain the normal resume path.
constexpr bool NativeBoundaryNeedsReferenceSubmission(bool frame_open,
    bool reference_continuation_required,bool resume_reference_query) {
  return !frame_open||reference_continuation_required||resume_reference_query;
}

// The latch represents accepted work, independently of the current packet or
// provider's failure flag. Never clear it until the completion callback returns
// successfully; provider removal and shutdown obey the same rule.
class NativeCompletionState {
 public:
  void AcceptPending() { pending_ = true; }
  bool pending() const { return pending_; }
  template<class Completion> void Complete(Completion&& complete, bool force = false) {
    if(!pending_&&!force)return;
    if(completing_)throw std::runtime_error("Recursive native completion");
    completing_=true;
    try {std::forward<Completion>(complete)();}
    catch(...) {completing_=false;throw;}
    completing_=false;
    pending_=false;
  }
 private:
  bool pending_=false;
  bool completing_=false;
};

constexpr bool NativeScratchWriteNeedsCompletion(uint32_t index,uint32_t mask) {
  return index>=0x578&&index<=0x57f&&(mask&(1u<<(index-0x578)));
}

// Called only for type-3 packets whose predicate passed. Permit only decoder
// operations that cannot publish completion or touch externally visible memory.
// Register packets still pass through WriteRegister's scratch-write barrier.
// Unknown operations are conservative completion boundaries.
constexpr bool NativePacketNeedsCompletion(uint32_t opcode, uint32_t count) {
  switch (opcode) {
    case 0x10:  // NOP.
    case 0x22:  // DRAW_INDX: the draw provider selects ownership.
    case 0x36:  // DRAW_INDX_2.
    case 0x2d:  // SET_CONSTANT.
    case 0x55:  // SET_CONSTANT2.
    case 0x56:  // SET_SHADER_CONSTANTS.
    case 0x27:  // IM_LOAD: immutable shader input, no guest publication.
    case 0x2b:  // IM_LOAD_IMMEDIATE.
    case 0x2f:  // LOAD_ALU_CONSTANT: register load, no guest publication.
    case 0x37:  // INDIRECT_BUFFER_PFD: nested packets retain their barriers.
    case 0x3f:  // INDIRECT_BUFFER.
    case 0x50:  // SET_BIN_MASK.
    case 0x51:  // SET_BIN_SELECT.
    case 0x60:  // SET_BIN_MASK_LO.
    case 0x61:  // SET_BIN_MASK_HI.
    case 0x62:  // SET_BIN_SELECT_LO.
    case 0x63:  // SET_BIN_SELECT_HI.
      return false;
    case 0x3b:  // Count-one INVALIDATE_STATE is a decoder no-op.
      return count != 1;
    case 0x46:  // Count-one EVENT_WRITE only writes VGT_EVENT_INITIATOR.
      return count != 1;
    default:
      return true;
  }
}

}  // namespace rex::graphics
