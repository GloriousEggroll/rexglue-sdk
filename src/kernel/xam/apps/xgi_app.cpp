/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/network.h>
#include <rex/system/xam/user_profile.h>
#include <rex/thread.h>

#include <cstring>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

namespace {
using rex::system::xam::Network;

constexpr uint32_t kSessionCreateHost = 0x1;
constexpr X_HRESULT kInsufficientBuffer = 0x8007007A;

void ReadInfo(const uint8_t* p, rex::system::xam::XSessionInfo& info) {
  std::memcpy(info.id.ab.data(), p, 8);
  std::memcpy(info.host.bytes.data(), p + 8, 36);
  std::memcpy(info.key.ab.data(), p + 44, 16);
}
void WriteInfo(uint8_t* p, const rex::system::xam::XSessionInfo& info) {
  std::memcpy(p, info.id.ab.data(), 8);
  std::memcpy(p + 8, info.host.bytes.data(), 36);
  std::memcpy(p + 44, info.key.ab.data(), 16);
}
// XSESSION_SEARCHRESULT_HEADER with no results: System Link sessions are
// found by the title's own broadcasts, not by Live matchmaking.
void WriteNoSearchResults(memory::Memory* memory, uint32_t header_ptr) {
  if (!header_ptr) return;
  auto* header = memory->TranslateVirtual(header_ptr);
  memory::store_and_swap<uint32_t>(header + 0, 0);
  memory::store_and_swap<uint32_t>(header + 4, 0);
}
}  // namespace

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      Network::Get().SetContext(context_id, context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      REXKRNL_DEBUG("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
                    achievements_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_INFO(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);
      auto& network = Network::Get();
      rex::system::xam::XSessionInfo info;
      const bool host = flags & kSessionCreateHost;
      if (host) {
        // A System Link session: this machine's address, a new ID and key.
        info.id = network.CreateSystemLinkKid();
        info.host = network.local_xnaddr();
        network.Random(info.key.ab.data(), info.key.ab.size());
        if (session_info_ptr) WriteInfo(memory_->TranslateVirtual(session_info_ptr), info);
      } else if (session_info_ptr) {
        ReadInfo(memory_->TranslateVirtual(session_info_ptr), info);
        network.RememberPeer(info.host);
      }
      network.RegisterKey(info.id, info.key);
      uint64_t nonce;
      network.Random(reinterpret_cast<uint8_t*>(&nonce), sizeof(nonce));
      if (nonce_ptr) memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), nonce);
      const bool known = network.WithSession(session_ptr, [&](Network::Session& session) {
        session.flags = flags;
        session.user_index = user_xuid;
        session.max_public = num_slots_public;
        session.max_private = num_slots_private;
        session.host = host;
        session.state = 0;
        session.nonce = nonce;
        session.info = info;
        session.members.clear();
      });
      if (!known) REXKRNL_WARN("XGISessionCreateImpl: unknown session object {:08X}", session_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_INFO("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      Network::Get().DeleteSession(obj_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t unk_0 = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);

      assert_zero(unk_0);
      REXKRNL_INFO("XGISessionJoinLocal({:08X}, {}, {}, {:08X}, {:08X})", session_ptr, user_count,
                   unk_0, user_index_array, private_slots_array);
      const uint64_t xuid = kernel_state_->user_profile()->xuid();
      Network::Get().WithSession(session_ptr, [&](Network::Session& session) {
        for (uint32_t i = 0; i < user_count && i < 4; ++i) {
          Network::Session::Member member;
          member.user_index = user_index_array
              ? memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_index_array + i * 4))
              : 0;
          member.xuid = member.user_index == 0 ? xuid : 0;
          member.private_slot = private_slots_array &&
              memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(private_slots_array + i * 4));
          session.members.push_back(member);
        }
      });
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      // XSessionJoinRemote(session, count, xuids, private slots)
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuid_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 12);
      REXKRNL_INFO("XGISessionJoinRemote({:08X}, {}, {:08X}, {:08X})", session_ptr, xuid_count,
                   xuids_ptr, private_slots_array);
      Network::Get().WithSession(session_ptr, [&](Network::Session& session) {
        for (uint32_t i = 0; i < xuid_count && i < 32 && xuids_ptr; ++i) {
          Network::Session::Member member;
          member.xuid = memory::load_and_swap<uint64_t>(memory_->TranslateVirtual(xuids_ptr + i * 8));
          member.private_slot = private_slots_array &&
              memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(private_slots_array + i * 4));
          session.members.push_back(member);
        }
      });
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_INFO("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      Network::Get().WithSession(obj_ptr, [](Network::Session& session) { session.state = 2; });
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_INFO("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      Network::Get().WithSession(obj_ptr, [](Network::Session& session) { session.state = 0; });
      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      WriteNoSearchResults(memory_, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_INFO("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                   maxPublicSlots, maxPrivateSlots);
      Network::Get().WithSession(obj_ptr, [&](Network::Session& session) {
        session.flags = (session.flags & kSessionCreateHost) | (flags & ~kSessionCreateHost);
        session.max_public = maxPublicSlots;
        session.max_private = maxPrivateSlots;
      });
      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);
      WriteNoSearchResults(memory_, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);
      // details_buffer_size points at the buffer size. XSESSION_LOCAL_DETAILS
      // is 128 bytes, followed here by the XSESSION_MEMBER array (16 bytes each).
      X_HRESULT result = X_E_SUCCESS;
      const bool known = Network::Get().WithSession(obj_ptr, [&](Network::Session& session) {
        const uint32_t required = 128 + uint32_t(session.members.size()) * 16;
        auto* size = details_buffer_size ? memory_->TranslateVirtual(details_buffer_size) : nullptr;
        if (!size || !session_details_ptr || memory::load_and_swap<uint32_t>(size) < required) {
          if (size) memory::store_and_swap<uint32_t>(size, required);
          result = kInsufficientBuffer;
          return;
        }
        auto* d = memory_->TranslateVirtual(session_details_ptr);
        std::memset(d, 0, required);
        uint32_t used_public = 0, used_private = 0;
        for (const auto& member : session.members) (member.private_slot ? used_private : used_public)++;
        const uint32_t words[] = {
            session.user_index, Network::Get().Context(0x0000800A),
            Network::Get().Context(0x0000800B), session.flags, session.max_public,
            session.max_private, session.max_public > used_public ? session.max_public - used_public : 0,
            session.max_private > used_private ? session.max_private - used_private : 0,
            uint32_t(session.members.size()), uint32_t(session.members.size()), session.state};
        for (size_t i = 0; i < std::size(words); ++i) memory::store_and_swap<uint32_t>(d + i * 4, words[i]);
        memory::store_and_swap<uint64_t>(d + 48, session.nonce);
        WriteInfo(d + 56, session.info);
        const uint32_t members_ptr = session_details_ptr + 128;
        memory::store_and_swap<uint32_t>(d + 124, members_ptr);
        for (size_t i = 0; i < session.members.size(); ++i) {
          auto* m = d + 128 + i * 16;
          memory::store_and_swap<uint64_t>(m, session.members[i].xuid);
          memory::store_and_swap<uint32_t>(m + 8, session.members[i].user_index);
          memory::store_and_swap<uint32_t>(m + 12, session.members[i].private_slot ? 1 : 0);
        }
      });
      return known ? result : X_E_FAIL;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_INFO("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                   session_info_ptr, user_index, reserved1, reserved2, reserved3);
      // This machine becomes the host when a local user index is given;
      // otherwise the title supplies the new host's session information.
      Network::Get().WithSession(obj_ptr, [&](Network::Session& session) {
        if (user_index != 0xFE) {
          session.host = true;
          session.info.host = Network::Get().local_xnaddr();
          if (session_info_ptr) WriteInfo(memory_->TranslateVirtual(session_info_ptr), session.info);
        } else if (session_info_ptr) {
          session.host = false;
          ReadInfo(memory_->TranslateVirtual(session_info_ptr), session.info);
          Network::Get().RememberPeer(session.info.host);
        }
      });
      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      WriteNoSearchResults(memory_, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      WriteNoSearchResults(memory_, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
