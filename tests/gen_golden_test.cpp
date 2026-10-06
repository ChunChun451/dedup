// Golden tests for dedup-gen: the datasets must be bit-for-bit reproducible forever (DECISIONS.md D22).
// If a golden hash changes, every generated dataset changes: only update it on purpose, with a new decision.
#include <blake3.h>
#include <gtest/gtest.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "chacha20.hpp"
#include "gen.hpp"

namespace dedup::gen {
namespace {

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, "%02x", p[i]);
    s += b;
  }
  return s;
}

std::string Blake3Hex(const std::vector<uint8_t>& v) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, v.data(), v.size());
  uint8_t out[32];
  blake3_hasher_finalize(&h, out, sizeof out);
  return Hex(out, 16);  // 128 bits is plenty to detect a change
}

TEST(ChaCha20, Rfc8439BlockVector) {
  // RFC 8439 section 2.3.2: key 00..1f, counter 1, nonce 00:00:00:09:00:00:00:4a:00:00:00:00.
  const uint32_t in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0x03020100, 0x07060504,
                           0x0b0a0908, 0x0f0e0d0c, 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
                           0x00000001, 0x09000000, 0x4a000000, 0x00000000};
  const uint32_t want[16] = {0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3, 0xc7f4d1c7, 0x0368c033,
                             0x9aaa2204, 0x4e6cd4c3, 0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
                             0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2};
  uint32_t out[16];
  ChaCha20Block(in, out);
  for (int i = 0; i < 16; ++i) EXPECT_EQ(out[i], want[i]) << "word " << i;
}

constexpr uint64_t kOddSize = 3 * kBlockSize + 12345;  // ends with a partial block

struct Golden {
  Mode mode;
  const char* name;
  const char* hash;  // BLAKE3 (first 16 bytes) of Generate(mode, seed 1, kOddSize)
};
constexpr std::array<Golden, 4> kGolden = {{
    {Mode::kRandom, "random", "2f9165b7e5a9ff4e1f5c251fa65a228e"},
    {Mode::kText, "text", "165f7da73257a45b16e50a6ddae98865"},
    {Mode::kStructured, "structured", "9dfc69de4f87c3754ee04dd9178f0798"},
    {Mode::kMixed, "mixed", "823e0684818ffacdb0e1203faa53fa9d"},
}};

TEST(Generate, GoldenHashes) {
  for (const auto& g : kGolden) {
    EXPECT_EQ(Blake3Hex(GenerateToVector(g.mode, 1, kOddSize, 1)), g.hash) << g.name;
  }
}

TEST(Generate, ThreadCountDoesNotMatter) {
  for (const auto& g : kGolden) {
    EXPECT_EQ(GenerateToVector(g.mode, 1, kOddSize, 1), GenerateToVector(g.mode, 1, kOddSize, 8)) << g.name;
  }
}

TEST(Generate, PrefixesAgree) {
  // Block i depends only on (mode, seed, i): a shorter file is a prefix of a longer one.
  for (const auto& g : kGolden) {
    auto longer = GenerateToVector(g.mode, 1, kOddSize, 4);
    auto shorter = GenerateToVector(g.mode, 1, kBlockSize + 77, 4);
    EXPECT_TRUE(std::equal(shorter.begin(), shorter.end(), longer.begin())) << g.name;
  }
}

TEST(Generate, SeedsDiffer) {
  for (const auto& g : kGolden) {
    EXPECT_NE(GenerateToVector(g.mode, 1, 4096, 1), GenerateToVector(g.mode, 2, 4096, 1)) << g.name;
  }
}

double ZstdRatio(const std::vector<uint8_t>& v) {
  std::vector<uint8_t> out(ZSTD_compressBound(v.size()));
  size_t n = ZSTD_compress(out.data(), out.size(), v.data(), v.size(), 1);
  return double(v.size()) / double(n);
}

TEST(Generate, CompressibilityPerMode) {
  // Measured on 256 MiB at G1/W02.1: random 1.00, text 2.76, structured 3.52, mixed 1.96 (zstd level 1).
  const uint64_t n = 8 * kBlockSize;
  EXPECT_LT(ZstdRatio(GenerateToVector(Mode::kRandom, 1, n, 8)), 1.01);
  EXPECT_GT(ZstdRatio(GenerateToVector(Mode::kText, 1, n, 8)), 2.4);
  EXPECT_GT(ZstdRatio(GenerateToVector(Mode::kStructured, 1, n, 8)), 3.0);
  double mixed = ZstdRatio(GenerateToVector(Mode::kMixed, 1, n, 8));
  EXPECT_GT(mixed, 1.6);
  EXPECT_LT(mixed, 2.4);
}

MutateStats MutateVector(const std::vector<uint8_t>& in, uint64_t seed, uint32_t ppm, std::vector<uint8_t>& out,
                         size_t read_size) {
  size_t pos = 0;
  return Mutate(
      seed, ppm,
      [&](uint8_t* p, size_t cap) {
        size_t k = std::min({cap, read_size, in.size() - pos});
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(pos), k, p);
        pos += k;
        return k;
      },
      [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
}

TEST(Mutate, GoldenAndRate) {
  auto in = GenerateToVector(Mode::kRandom, 7, 256 * kBlockSize, 8);
  std::vector<uint8_t> out;
  auto st = MutateVector(in, 9, 5000, out, 4 << 20);
  EXPECT_EQ(Blake3Hex(out), "5400d1fc202a1d18f6918ff500e789fb");
  EXPECT_EQ(st.input_bytes, in.size());
  EXPECT_EQ(st.output_bytes, out.size());
  EXPECT_EQ(st.output_bytes, st.input_bytes + st.inserted - st.deleted);
  EXPECT_GT(st.EditFraction(), 0.004);  // 0.5% target; ~330 edits, so +-20% is > 3 standard deviations
  EXPECT_LT(st.EditFraction(), 0.006);
}

TEST(Mutate, ReadSizeDoesNotMatter) {
  auto in = GenerateToVector(Mode::kText, 3, 16 * kBlockSize, 8);
  std::vector<uint8_t> a, b;
  MutateVector(in, 5, 20000, a, 4 << 20);
  MutateVector(in, 5, 20000, b, 4093);  // odd read size: edits and skips cross read boundaries
  EXPECT_EQ(a, b);
}

TEST(Mutate, ZeroRateCopies) {
  auto in = GenerateToVector(Mode::kMixed, 3, 2 * kBlockSize + 5, 2);
  std::vector<uint8_t> out;
  auto st = MutateVector(in, 5, 0, out, 1 << 20);
  EXPECT_EQ(out, in);
  EXPECT_EQ(st.edits, 0u);
}

}  // namespace
}  // namespace dedup::gen
