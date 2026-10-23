#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <rex/filesystem.h>
#include <cstdlib>
#include <stdexcept>

#if !REX_PLATFORM_WIN32
#include <unistd.h>

namespace {
struct TemporaryFile {
  std::string path =
      (std::filesystem::temp_directory_path() / "rex-file-access-XXXXXX").string();
  TemporaryFile() {
    const int fd = mkstemp(path.data());
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    close(fd);
  }
  ~TemporaryFile() { std::filesystem::remove(path); }
};
}  // namespace

TEST_CASE("FileHandle supports requested read and write access", "[filesystem][file_access]") {
  using rex::filesystem::FileAccess;
  const uint32_t access = GENERATE(
      FileAccess::kGenericRead | FileAccess::kGenericWrite,
      FileAccess::kFileReadData | FileAccess::kFileWriteData,
      FileAccess::kGenericRead | FileAccess::kFileWriteData,
      FileAccess::kFileReadData | FileAccess::kGenericWrite,
      uint32_t(FileAccess::kGenericAll),
      FileAccess::kGenericAll | FileAccess::kGenericWrite);
  CAPTURE(access);
  TemporaryFile temporary;
  auto file = rex::filesystem::FileHandle::OpenExisting(temporary.path, access);
  REQUIRE(file);
  size_t transferred = 0;
  REQUIRE(file->Write(0, "abc", 3, &transferred));
  REQUIRE(transferred == 3);
  char data[3]{};
  REQUIRE(file->Read(0, data, sizeof(data), &transferred));
  REQUIRE(transferred == 3);
  CHECK(std::string(data, 3) == "abc");
  REQUIRE(file->Write(1, "Z", 1, &transferred));
  REQUIRE(file->Read(0, data, sizeof(data), &transferred));
  CHECK(std::string(data, 3) == "aZc");
}
#endif
