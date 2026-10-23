#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/cache.h>

using namespace rex::graphics;

namespace {

struct TextureCacheTestAccessor : TextureCache {
  using TextureCache::GetIntegerScaleBits;
};

constexpr uint8_t PackSigns(xenos::TextureSign x, xenos::TextureSign y,
                            xenos::TextureSign z, xenos::TextureSign w) {
  return uint8_t(uint32_t(x) | (uint32_t(y) << 2) | (uint32_t(z) << 4) |
                 (uint32_t(w) << 6));
}

}  // namespace

TEST_CASE("Texture integer scale follows format widths and fetch signs",
          "[graphics][texture]") {
  constexpr uint32_t rgba_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  constexpr uint8_t unsigned_signs =
      PackSigns(xenos::TextureSign::kUnsigned, xenos::TextureSign::kUnsigned,
                xenos::TextureSign::kUnsigned, xenos::TextureSign::kUnsigned);

  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_8_8_8_8, 0, rgba_swizzle,
            unsigned_signs) == 0);
  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_16_FLOAT, 1, rgba_swizzle,
            unsigned_signs) == 0);
  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_8_8_8_8, 1, rgba_swizzle,
            unsigned_signs) ==
        (7u | (7u << 5) | (7u << 10) | (7u << 15)));

  constexpr uint8_t signed_red =
      PackSigns(xenos::TextureSign::kSigned, xenos::TextureSign::kUnsigned,
                xenos::TextureSign::kUnsigned, xenos::TextureSign::kUnsigned);
  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_8_8_8_8, 1, rgba_swizzle,
            signed_red) ==
        (23u | (7u << 5) | (7u << 10) | (7u << 15)));

  constexpr uint8_t gamma_red =
      PackSigns(xenos::TextureSign::kGamma, xenos::TextureSign::kUnsigned,
                xenos::TextureSign::kUnsigned, xenos::TextureSign::kUnsigned);
  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_8_8_8_8, 1, rgba_swizzle,
            gamma_red) == ((7u << 5) | (7u << 10) | (7u << 15)));

  constexpr uint32_t argb_swizzle = XE_GPU_MAKE_TEXTURE_SWIZZLE(A, R, G, B);
  CHECK(TextureCacheTestAccessor::GetIntegerScaleBits(
            xenos::TextureFormat::k_2_10_10_10, 1, argb_swizzle,
            unsigned_signs) ==
        (1u | (9u << 5) | (9u << 10) | (9u << 15)));
}
