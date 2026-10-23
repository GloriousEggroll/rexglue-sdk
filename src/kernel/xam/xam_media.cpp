/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xam {

using namespace rex::system;

u32 XamMediaVerificationCreate_entry(u32 creation_flags) {
  return X_ERROR_SUCCESS;
}

u32 XamMediaVerificationClose_entry() {
  return X_ERROR_SUCCESS;
}

u32 XamMediaVerificationVerify_entry(u32 unknown, ppc_ptr_t<XAM_OVERLAPPED> overlapped,
                                     u32 callback) {
  if (callback) {
    XThread::GetCurrentThread()->EnqueueApc(callback & ~1u, 0, 0, 0);
  }
  return X_ERROR_SUCCESS;
}

struct X_SECURITY_FAILURE_INFORMATION {
  be<u32> size;
  be<u32> failed_reads;
  be<u32> failed_hashes;
  be<u32> blocks_checked;
  be<u32> total_blocks;
  be<u32> complete;
};
static_assert(sizeof(X_SECURITY_FAILURE_INFORMATION) == 24);

u32 XamMediaVerificationFailedBlocks_entry(
    ppc_ptr_t<X_SECURITY_FAILURE_INFORMATION> failure_information) {
  if (!failure_information) {
    return X_ERROR_SUCCESS;
  }
  if (failure_information->size != sizeof(X_SECURITY_FAILURE_INFORMATION)) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }

  failure_information->failed_reads = 0;
  failure_information->failed_hashes = 0;
  failure_information->blocks_checked = 0;
  failure_information->total_blocks = 0;
  failure_information->complete = 1;
  return X_ERROR_SUCCESS;
}

u32 XamMediaVerificationInject_entry(u32 unknown_1, u32 unknown_2) {
  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xam

REX_EXPORT(__imp__XamMediaVerificationCreate,
           rex::kernel::xam::XamMediaVerificationCreate_entry)
REX_EXPORT(__imp__XamMediaVerificationClose,
           rex::kernel::xam::XamMediaVerificationClose_entry)
REX_EXPORT(__imp__XamMediaVerificationVerify,
           rex::kernel::xam::XamMediaVerificationVerify_entry)
REX_EXPORT(__imp__XamMediaVerificationFailedBlocks,
           rex::kernel::xam::XamMediaVerificationFailedBlocks_entry)
REX_EXPORT(__imp__XamMediaVerificationInject,
           rex::kernel::xam::XamMediaVerificationInject_entry)
