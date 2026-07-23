/**
 * @file        tests/unit/codegen/gapfill_test.cpp
 * @brief       Unit tests for speculative function segmentation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/test_support.h>

#include "codegen/phase_helpers.h"

#include <array>
namespace rex::codegen {

TEST_CASE("GapFill splits functions after an unconditional bctr", "[codegen][gapfill]") {
  constexpr uint32_t kBase = 0x82000000;
  constexpr std::array<uint8_t, 16> kCode = {
      0x81, 0x43, 0x01, 0xDC,  // lwz r10,476(r3)
      0x7D, 0x49, 0x03, 0xA6,  // mtctr r10
      0x4E, 0x80, 0x04, 0x20,  // bctr
      0x4E, 0x80, 0x00, 0x20,  // blr
  };

  TestModule module;
  module.Load(kBase, kCode.data(), kCode.size());
  auto binary = BinaryView::fromModule(module);

  FunctionGraph graph;
  auto segments = splitCodeRegionOnTerminators(
      {kBase, kBase + static_cast<uint32_t>(kCode.size())}, binary, graph);

  REQUIRE(segments.size() == 2);
  CHECK(segments[0].start == kBase);
  CHECK(segments[0].end == kBase + 12);
  CHECK(segments[1].start == kBase + 12);
  CHECK(segments[1].end == kBase + 16);
}

TEST_CASE("GapFill sees functions registered earlier in the pass", "[codegen][gapfill]") {
  constexpr uint32_t kBase = 0x82000000;
  constexpr std::array<uint8_t, 12> kCode = {
      0x4E, 0x80, 0x00, 0x20,  // blr
      0x4B, 0xFF, 0xFF, 0xFC,  // b kBase
      0x4E, 0x80, 0x00, 0x20,  // blr
  };

  TestModule module;
  module.Load(kBase, kCode.data(), kCode.size());
  auto binary = BinaryView::fromModule(module);
  FunctionGraph graph;

  auto targetSegments =
      splitCodeRegionOnTerminators({kBase, kBase + 4}, binary, graph);
  REQUIRE(targetSegments.size() == 1);
  graph.addFunction(targetSegments[0].start, targetSegments[0].size(),
                    FunctionAuthority::GAP_FILL);

  auto callerSegments =
      splitCodeRegionOnTerminators({kBase + 4, kBase + 12}, binary, graph);
  REQUIRE(callerSegments.size() == 2);
  CHECK(callerSegments[0].start == kBase + 4);
  CHECK(callerSegments[0].end == kBase + 8);
  CHECK(callerSegments[1].start == kBase + 8);
  CHECK(callerSegments[1].end == kBase + 12);
}

}  // namespace rex::codegen
