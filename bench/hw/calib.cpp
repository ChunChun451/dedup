// calib: a fixed ~2 s single-core integer workload; prints its speed (SPEC.md 6.8, run before every benchmark cell).
// A lower score than the session's first one means the CPU is slower right now (heat, power mode, background load).
#include <chrono>
#include <cstdint>
#include <cstdio>

int main() {
  constexpr uint64_t kSteps = 2'000'000'000;
  uint64_t x = 1;
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < kSteps; ++i) {
    x = x * 3 + i;
    asm volatile("" : "+r"(x));  // keep the dependent chain: one step cannot start before the previous ends
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("{\"score\": %.4f, \"seconds\": %.3f, \"sink\": %llu}\n", kSteps / s / 1e9, s,
              static_cast<unsigned long long>(x & 1));
  return 0;
}
