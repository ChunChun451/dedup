// Smoke tests: prove each pinned dependency builds, links and gives known answers.
#include <blake3.h>
#include <gtest/gtest.h>
#include <zstd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string ToHex(const uint8_t* p, size_t n) {
  std::string s;
  char buf[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%02x", p[i]);
    s += buf;
  }
  return s;
}

std::string Blake3Hex(const void* data, size_t len) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data, len);
  std::array<uint8_t, BLAKE3_OUT_LEN> out{};
  blake3_hasher_finalize(&h, out.data(), out.size());
  return ToHex(out.data(), out.size());
}

TEST(Blake3, EmptyInputMatchesOfficialVector) {
  // From BLAKE3 test_vectors.json, input_len = 0.
  EXPECT_EQ(Blake3Hex("", 0), "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST(Blake3, VersionIsPinned) { EXPECT_STREQ(blake3_version(), "1.8.7"); }

TEST(Zstd, RoundTrip) {
  std::vector<uint8_t> input(1 << 20);
  for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>((i * 7) % 251);

  std::vector<uint8_t> packed(ZSTD_compressBound(input.size()));
  size_t n = ZSTD_compress(packed.data(), packed.size(), input.data(), input.size(), 1);
  ASSERT_FALSE(ZSTD_isError(n)) << ZSTD_getErrorName(n);
  EXPECT_LT(n, input.size() / 10);  // repeating pattern: must shrink a lot

  std::vector<uint8_t> back(input.size());
  size_t m = ZSTD_decompress(back.data(), back.size(), packed.data(), n);
  ASSERT_FALSE(ZSTD_isError(m)) << ZSTD_getErrorName(m);
  EXPECT_EQ(m, input.size());
  EXPECT_EQ(back, input);
}

TEST(Zstd, VersionIsPinned) { EXPECT_EQ(ZSTD_versionNumber(), 10507u); }

}  // namespace
