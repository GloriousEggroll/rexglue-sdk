#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rex/graphics/pipeline/shader/shader.h"
#include "rex/string/buffer.h"

namespace {

struct XshEntryHeader {
  uint64_t hash;
  uint32_t count_and_type;
};

static_assert(sizeof(XshEntryHeader) == 16);

}  // namespace

TEST_CASE("Dump XSH guest shaders", "[xsh-diagnostic]") {
  const char* input_path = std::getenv("REX_XSH_DUMP_INPUT");
  const char* output_path = std::getenv("REX_XSH_DUMP_OUTPUT");
  if (!input_path || !output_path) {
    SKIP("Set REX_XSH_DUMP_INPUT and REX_XSH_DUMP_OUTPUT");
  }

  std::ifstream input(input_path, std::ios::binary);
  REQUIRE(input);
  uint32_t magic = 0;
  uint32_t version = 0;
  input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  input.read(reinterpret_cast<char*>(&version), sizeof(version));
  REQUIRE(magic == UINT32_C(0x48534558));

  std::filesystem::create_directories(output_path);
  rex::string::StringBuffer disassembly_buffer;
  size_t entry_count = 0;
  while (input.peek() != std::ifstream::traits_type::eof()) {
    uint64_t hash = 0;
    uint32_t count_and_type = 0;
    input.read(reinterpret_cast<char*>(&hash), sizeof(hash));
    input.read(reinterpret_cast<char*>(&count_and_type), sizeof(count_and_type));
    REQUIRE(input);

    const uint32_t dword_count = count_and_type & UINT32_C(0x7FFFFFFF);
    const auto shader_type =
        count_and_type >> 31 ? rex::graphics::xenos::ShaderType::kPixel
                             : rex::graphics::xenos::ShaderType::kVertex;
    std::vector<uint32_t> ucode(dword_count);
    input.read(reinterpret_cast<char*>(ucode.data()),
               std::streamsize(ucode.size() * sizeof(uint32_t)));
    REQUIRE(input);

    rex::graphics::Shader shader(shader_type, hash, ucode.data(), ucode.size(),
                                 std::endian::big);
    shader.AnalyzeUcode(disassembly_buffer);
    shader.DumpUcode(output_path);
    ++entry_count;
  }
  REQUIRE(entry_count != 0);
}
