// ChaCha20 keystream (RFC 8439 block function) used as a deterministic, seekable random source for dedup-gen.
// State layout: words 0-3 constants, 4-11 key, 12-13 64-bit block counter, 14 stream tag, 15 sub-stream id.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dedup::gen {

inline uint32_t Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// One ChaCha20 block: out[i] = rounds(in)[i] + in[i].
inline void ChaCha20Block(const uint32_t in[16], uint32_t out[16]) {
  uint32_t x[16];
  std::memcpy(x, in, sizeof x);
  auto qr = [&x](int a, int b, int c, int d) {
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 7);
  };
  for (int i = 0; i < 10; ++i) {
    qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
    qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
  }
  for (int i = 0; i < 16; ++i) out[i] = x[i] + in[i];
}

// Keystream for (seed, tag, sub), starting at 64-byte block `counter`. Little-endian byte order.
class ChaCha {
 public:
  ChaCha(uint64_t seed, uint32_t tag, uint32_t sub, uint64_t counter = 0) {
    static constexpr uint32_t kInit[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                                           0, 0, 0x75646564, 0x65672d70,  // "dedup-ge"
                                           0x3176206e, 0, 0, 0,           // "n v1"
                                           0, 0, 0, 0};
    std::memcpy(state_, kInit, sizeof state_);
    state_[4] = static_cast<uint32_t>(seed);
    state_[5] = static_cast<uint32_t>(seed >> 32);
    state_[12] = static_cast<uint32_t>(counter);
    state_[13] = static_cast<uint32_t>(counter >> 32);
    state_[14] = tag;
    state_[15] = sub;
  }

  void Fill(uint8_t* p, size_t n) {
    while (n > 0) {
      if (pos_ == 64) Refill();
      size_t k = n < 64 - pos_ ? n : 64 - pos_;
      std::memcpy(p, buf_ + pos_, k);
      pos_ += k;
      p += k;
      n -= k;
    }
  }
  uint32_t U32() {
    uint8_t b[4];
    Fill(b, 4);
    return uint32_t{b[0]} | uint32_t{b[1]} << 8 | uint32_t{b[2]} << 16 | uint32_t{b[3]} << 24;
  }
  uint64_t U64() {
    const uint64_t lo = U32();  // separate statements: operand order inside one expression is unspecified in C++
    const uint64_t hi = U32();
    return lo | hi << 32;
  }
  // Uniform-ish integer in [0, n). Multiply-shift: bias < n / 2^32, irrelevant here and fully deterministic.
  uint32_t Below(uint32_t n) { return static_cast<uint32_t>((uint64_t{U32()} * n) >> 32); }
  uint64_t Below64(uint64_t n) { return n == 0 ? 0 : U64() % n; }

 private:
  void Refill() {
    uint32_t out[16];
    ChaCha20Block(state_, out);
    for (int i = 0; i < 16; ++i) {
      buf_[4 * i] = static_cast<uint8_t>(out[i]);
      buf_[4 * i + 1] = static_cast<uint8_t>(out[i] >> 8);
      buf_[4 * i + 2] = static_cast<uint8_t>(out[i] >> 16);
      buf_[4 * i + 3] = static_cast<uint8_t>(out[i] >> 24);
    }
    if (++state_[12] == 0) ++state_[13];
    pos_ = 0;
  }

  uint32_t state_[16];
  uint8_t buf_[64];
  size_t pos_ = 64;
};

}  // namespace dedup::gen
