// membw: measure memory read bandwidth with N threads (roadmap W01.4, ceilings M1 / M16).
// Usage: membw <threads> [gib=4]   -> prints one JSON object.
// Each thread repeatedly reads its own slice of a buffer much larger than L3 (16 MiB), using AVX2 loads.
// Threads 0..7 go to different physical cores (CPUs 0,2,..,14); threads 8..15 to their SMT siblings.
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr int kReps = 7;
constexpr int kPassesPerRep = 4;

void PinTo(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

int CpuFor(int thread) { return thread < 8 ? thread * 2 : (thread - 8) * 2 + 1; }

__attribute__((target("avx2"))) uint64_t ReadSlice(const uint8_t* p, size_t n) {
  __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
  for (size_t i = 0; i < n; i += 128) {
    a0 = _mm256_xor_si256(a0, _mm256_load_si256(reinterpret_cast<const __m256i*>(p + i)));
    a1 = _mm256_xor_si256(a1, _mm256_load_si256(reinterpret_cast<const __m256i*>(p + i + 32)));
    a2 = _mm256_xor_si256(a2, _mm256_load_si256(reinterpret_cast<const __m256i*>(p + i + 64)));
    a3 = _mm256_xor_si256(a3, _mm256_load_si256(reinterpret_cast<const __m256i*>(p + i + 96)));
  }
  __m256i x = _mm256_xor_si256(_mm256_xor_si256(a0, a1), _mm256_xor_si256(a2, a3));
  return static_cast<uint64_t>(_mm256_extract_epi64(x, 0) ^ _mm256_extract_epi64(x, 3));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: membw <threads> [gib]\n");
    return 2;
  }
  const int threads = std::atoi(argv[1]);
  const size_t gib = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 4;
  if (threads < 1 || threads > 16 || gib < 1) {
    std::fprintf(stderr, "threads must be 1..16, gib >= 1\n");
    return 2;
  }
  const size_t total = gib << 30;
  const size_t slice = (total / threads) & ~size_t{4095};
  // 2 MiB alignment so the kernel can back the buffer with transparent huge pages (THP is in madvise mode here).
  auto* buf = static_cast<uint8_t*>(std::aligned_alloc(size_t{2} << 20, total));
  if (!buf) return 1;
  const bool huge = std::getenv("MEMBW_NO_HUGEPAGES") == nullptr;
  if (huge) madvise(buf, total, MADV_HUGEPAGE);

  std::atomic<uint64_t> sink{0};
  std::atomic<int> ready{0};
  std::atomic<int> go{-1};  // rep number to run; threads spin on it
  std::atomic<int> done{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      PinTo(CpuFor(t));
      uint8_t* mine = buf + t * slice;
      for (size_t i = 0; i < slice; i += 4096) mine[i] = static_cast<uint8_t>(i >> 12);  // first touch on own core
      ready.fetch_add(1);
      for (int rep = 0; rep < kReps; ++rep) {
        while (go.load(std::memory_order_acquire) < rep) std::this_thread::yield();
        uint64_t x = 0;
        for (int pass = 0; pass < kPassesPerRep; ++pass) {
          x += ReadSlice(mine, slice);
          asm volatile("" : "+r"(x) : : "memory");  // stop the compiler from merging or skipping passes
        }
        sink.fetch_xor(x);
        done.fetch_add(1, std::memory_order_acq_rel);
      }
    });
  }
  while (ready.load() < threads) std::this_thread::yield();

  std::vector<double> gbps;
  for (int rep = 0; rep < kReps; ++rep) {
    done.store(0);
    auto t0 = std::chrono::steady_clock::now();
    go.store(rep, std::memory_order_release);
    while (done.load(std::memory_order_acquire) < threads) std::this_thread::yield();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    gbps.push_back(double(slice) * threads * kPassesPerRep / s / 1e9);
  }
  for (auto& th : pool) th.join();
  std::free(buf);

  std::sort(gbps.begin(), gbps.end());
  std::printf("{\"threads\": %d, \"buffer_gib\": %zu, \"huge_pages\": %s, \"reps\": %d, \"best_gbps\": %.2f, \"median_gbps\": %.2f, \"sink\": %llu}\n",
              threads, gib, huge ? "true" : "false", kReps, gbps.back(), gbps[kReps / 2],
              static_cast<unsigned long long>(sink.load() & 1));
  return 0;
}
