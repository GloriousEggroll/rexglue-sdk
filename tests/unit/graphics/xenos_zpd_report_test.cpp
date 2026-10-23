#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/xenos.h>

using namespace rex::graphics::xenos;

TEST_CASE("Xenos ZPD report addresses are normalized", "[graphics][zpd]") {
  CHECK(XenosZPDReport::GetRecordBase(0x1234567Fu) == 0x12345660u);
  CHECK(XenosZPDReport::GetSlotBase(0x1234567Fu) == 0x12345640u);
  CHECK(XenosZPDReport::GetSlotBase(0x12345660u) ==
        XenosZPDReport::GetSlotBase(0x12345640u));
  CHECK(XenosZPDReport::GetBeginRecordBase(0x1234567Fu) == 0x12345660u);
  CHECK(XenosZPDReport::GetEndRecordBase(0x1234567Fu) == 0x12345640u);
  CHECK(XenosZPDReport::IsBeginRecord(0x1234567Fu));
  CHECK_FALSE(XenosZPDReport::IsEndRecord(0x1234567Fu));
  CHECK(XenosZPDReport::IsEndRecord(0x1234565Fu));
  CHECK_FALSE(XenosZPDReport::IsBeginRecord(0x1234565Fu));
}

TEST_CASE("Xenos ZPD report recognizes pending sentinels", "[graphics][zpd]") {
  xe_gpu_depth_sample_counts report = {};
  CHECK_FALSE(XenosZPDReport::HasPendingSentinel(&report));

  SECTION("little-endian value in an A lane") {
    report.ZPass_A = 0xEDFEFFFFu;
    CHECK(XenosZPDReport::HasPendingSentinel(&report));
  }

  SECTION("big-endian value in an A lane") {
    report.ZFail_A = 0xFFFFFEEDu;
    CHECK(XenosZPDReport::HasPendingSentinel(&report));
  }

  SECTION("little-endian value in a B lane") {
    report.ZPass_B = 0xEDFEFFFFu;
    CHECK(XenosZPDReport::HasPendingSentinel(&report));
  }

  SECTION("big-endian value in a B lane") {
    report.ZFail_B = 0xFFFFFEEDu;
    CHECK(XenosZPDReport::HasPendingSentinel(&report));
  }
}

TEST_CASE("Xenos ZPD report writes a complete sample count",
          "[graphics][zpd]") {
  xe_gpu_depth_sample_counts report = {};
  std::memset(&report, 0xFF, sizeof(report));

  XenosZPDReport::WriteSampleCount(&report, 73);

  CHECK(report.Total_A == 73);
  CHECK(report.Total_B == 0);
  CHECK(report.ZFail_A == 0);
  CHECK(report.ZFail_B == 0);
  CHECK(report.ZPass_A == 73);
  CHECK(report.ZPass_B == 0);
  CHECK(report.StencilFail_A == 0);
  CHECK(report.StencilFail_B == 0);
}
