// Microbenchmarks. Chunker, hash and index benchmarks are added by later roadmap steps.
#include <benchmark/benchmark.h>
#include <blake3.h>

#include <cstdint>
#include <vector>

namespace {

// Blake3/<bytes>: hash one buffer of the given size; reports bytes/second.
void Blake3(benchmark::State& state) {
  std::vector<uint8_t> buf(static_cast<size_t>(state.range(0)), 0x5a);
  uint8_t out[BLAKE3_OUT_LEN];
  for (auto _ : state) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, buf.data(), buf.size());
    blake3_hasher_finalize(&h, out, sizeof out);
    benchmark::DoNotOptimize(out);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(Blake3)->Arg(16 << 10)->Arg(1 << 20);

}  // namespace
