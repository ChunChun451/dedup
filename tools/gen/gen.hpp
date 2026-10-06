// dedup-gen: deterministic test data (SPEC.md section 5, datasets D3/D4; DECISIONS.md D22).
// The output depends only on (mode, seed, size): never on thread count, machine or time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace dedup::gen {

enum class Mode { kRandom, kText, kStructured, kMixed };

std::optional<Mode> ParseMode(std::string_view name);

// Content is produced in independent 1 MiB blocks; block i depends only on (mode, seed, i).
inline constexpr size_t kBlockSize = size_t{1} << 20;

using Sink = std::function<void(const uint8_t* data, size_t len)>;
using Source = std::function<size_t(uint8_t* data, size_t cap)>;  // returns 0 at end of input

// Write `size` bytes of `mode` data to `sink`, in order, using `threads` worker threads.
void Generate(Mode mode, uint64_t seed, uint64_t size, unsigned threads, const Sink& sink);
std::vector<uint8_t> GenerateToVector(Mode mode, uint64_t seed, uint64_t size, unsigned threads);

// Mutation: copies `in` to `out` with edits. Each edit is an insert, delete or overwrite (1/3 each) of 1..8192 bytes;
// the input distance between edits is uniform in [0, 2G) with G = 4096.5 / rate, so about `rate` of the input is edited.
struct MutateStats {
  uint64_t input_bytes = 0, output_bytes = 0, edits = 0, inserted = 0, deleted = 0, overwritten = 0;
  double EditFraction() const {
    return input_bytes ? double(inserted + deleted + overwritten) / double(input_bytes) : 0.0;
  }
};
inline constexpr uint32_t kMaxEditLen = 8192;
MutateStats Mutate(uint64_t seed, uint32_t rate_ppm, const Source& in, const Sink& out);

}  // namespace dedup::gen
