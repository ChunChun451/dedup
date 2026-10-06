#include "gen.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <string>
#include <thread>

#include "chacha20.hpp"

namespace dedup::gen {
namespace {

// Stream tags keep the modes' random streams independent of each other.
constexpr uint32_t kTagRandom = 1, kTagText = 2, kTagStructured = 3, kTagMixed = 4, kTagMutate = 5, kTagVocab = 6;
constexpr uint64_t kVocabSeed = 0x766f636162ULL;  // fixed: every dataset shares one "language"
constexpr size_t kVocabSize = 4096;

// ---------- text: Zipf-distributed pseudo-words with English letter frequencies ----------

struct Vocabulary {
  std::vector<std::string> words;
  std::vector<uint64_t> cdf;  // cumulative integer Zipf weights (s = 1): w_r = 2^32 / (r + 1)

  Vocabulary() {
    // Letter frequencies per 1000 (approximate English), a..z.
    static constexpr std::array<int, 26> kFreq = {82, 15, 28, 43, 127, 22, 20, 61, 70, 2,  8,  40, 24,
                                                  67, 75, 19, 1,  60, 63, 91, 28, 10, 24, 2,  20, 1};
    std::array<int, 26> cum{};
    int total = 0;
    for (int i = 0; i < 26; ++i) cum[i] = (total += kFreq[i]);
    ChaCha rng(kVocabSeed, kTagVocab, 0);
    words.reserve(kVocabSize);
    uint64_t acc = 0;
    for (size_t r = 0; r < kVocabSize; ++r) {
      // Each random draw gets its own statement: C++ leaves the order of operands and function arguments
      // unspecified, so draws inside one expression could differ between compilers (GCC vs Clang did, at W02.1).
      const size_t len_a = rng.Below(5);
      const size_t len = 2 + len_a + rng.Below(5);  // 2..10, peak at 6
      std::string w;
      for (size_t i = 0; i < len; ++i) {
        int x = static_cast<int>(rng.Below(static_cast<uint32_t>(total)));
        w += static_cast<char>('a' + (std::upper_bound(cum.begin(), cum.end(), x) - cum.begin()));
      }
      words.push_back(std::move(w));
      acc += (uint64_t{1} << 32) / (r + 1);
      cdf.push_back(acc);
    }
  }
  const std::string& Pick(ChaCha& rng) const {
    uint64_t x = rng.Below64(cdf.back());
    return words[std::upper_bound(cdf.begin(), cdf.end(), x) - cdf.begin()];
  }
};

const Vocabulary& Vocab() {
  static const Vocabulary v;
  return v;
}

// Writes into [p, end), truncating the last piece at `end`.
class Out {
 public:
  Out(uint8_t* p, uint8_t* end) : p_(p), end_(end) {}
  bool Full() const { return p_ == end_; }
  void Put(std::string_view s) {
    size_t k = std::min(s.size(), static_cast<size_t>(end_ - p_));
    std::memcpy(p_, s.data(), k);
    p_ += k;
  }
  void Put(char c) {
    if (p_ != end_) *p_++ = static_cast<uint8_t>(c);
  }
  void Random(ChaCha& rng, size_t n) {
    n = std::min(n, static_cast<size_t>(end_ - p_));
    rng.Fill(p_, n);
    p_ += n;
  }

 private:
  uint8_t* p_;
  uint8_t* end_;
};

void TextInto(ChaCha& rng, Out& out) {
  const Vocabulary& v = Vocab();
  while (!out.Full()) {
    uint32_t n = 4 + rng.Below(14);
    for (uint32_t i = 0; i < n && !out.Full(); ++i) {
      std::string w = v.Pick(rng);
      if (i == 0) w[0] = static_cast<char>(w[0] - 'a' + 'A');
      out.Put(w);
      if (i + 1 < n) out.Put(rng.Below(12) == 0 ? ", " : " ");
    }
    out.Put(rng.Below(20) == 0 ? '?' : '.');
    out.Put(rng.Below(6) == 0 ? "\n\n" : " ");
  }
}

// ---------- structured: log lines ----------

// Days since 1970-01-01 -> y-m-d (Howard Hinnant's civil_from_days, integer only).
void CivilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

void LogInto(ChaCha& rng, Out& out, int64_t& ms) {
  static constexpr const char* kServices[] = {"api",  "auth",   "billing", "cart",   "search", "media",
                                              "mail", "worker", "gateway", "notify", "report", "sync"};
  static constexpr const char* kPaths[] = {
      "/v1/items/",    "/v1/users/",     "/v1/orders/",   "/v1/carts/",     "/v1/search?q=", "/v1/files/",
      "/v2/items/",    "/v2/users/",     "/v2/orders/",   "/v2/invoices/",  "/v2/reports/",  "/v2/events/",
      "/health/",      "/metrics/",      "/admin/users/", "/admin/jobs/",   "/static/img/",  "/static/js/",
      "/auth/login/",  "/auth/refresh/", "/auth/logout/", "/sync/devices/", "/mail/send/",   "/notify/push/"};
  static constexpr const char* kHex = "0123456789abcdef";
  char line[256];
  while (!out.Full()) {
    ms += rng.Below(50);
    int64_t secs = ms / 1000;
    int y, mo, d;
    CivilFromDays(secs / 86400, y, mo, d);
    int sod = static_cast<int>(secs % 86400);
    // One statement per draw, in this fixed order (see the note in Vocabulary).
    const uint32_t host_a = rng.Below(64);
    const uint32_t host_b = rng.Below(64);
    const uint32_t host = std::min(host_a, host_b);
    const uint32_t svc = rng.Below(12);
    const uint32_t lv = rng.Below(100);
    const char* level = lv < 80 ? "INFO" : lv < 92 ? "WARN" : lv < 97 ? "ERROR" : "DEBUG";
    char req[17];
    for (int i = 0; i < 16; ++i) req[i] = kHex[rng.Below(16)];
    req[16] = 0;
    uint32_t mt = rng.Below(100);
    const char* method = mt < 70 ? "GET" : mt < 90 ? "POST" : mt < 96 ? "PUT" : "DELETE";
    uint32_t st = rng.Below(100);
    int status = st < 82 ? 200 : st < 87 ? 201 : st < 90 ? 204 : st < 94 ? 304 : st < 96 ? 400 : st < 99 ? 404 : 500;
    const uint32_t latency_cap = rng.Below(2000) + 1;
    const uint32_t latency = 1 + rng.Below(latency_cap);
    const uint32_t user = rng.Below(100000);
    const uint32_t path = rng.Below(24);
    const uint32_t item = rng.Below(10000);
    int n = std::snprintf(line, sizeof line,
                          "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ host-%02u %s[%u] %s req=%s user=%u %s %s%u %d %ums\n",
                          y, mo, d, sod / 3600, sod / 60 % 60, sod % 60, static_cast<int>(ms % 1000), host,
                          kServices[svc], 1000 + (host * 37 + svc * 11) % 30000, level, req, user, method,
                          kPaths[path], item, status, latency);
    out.Put(std::string_view(line, static_cast<size_t>(n)));
  }
}

constexpr int64_t kEpochMs = 1767225600000;  // 2026-01-01T00:00:00Z

void Block(Mode mode, uint64_t seed, uint64_t index, uint8_t* p, size_t n) {
  switch (mode) {
    case Mode::kRandom: {
      // A plain keystream: block i starts at ChaCha block i * 16384, so the file is seekable.
      ChaCha rng(seed, kTagRandom, 0, index * (kBlockSize / 64));
      rng.Fill(p, n);
      return;
    }
    case Mode::kText: {
      ChaCha rng(seed, kTagText, static_cast<uint32_t>(index));
      Out out(p, p + n);
      TextInto(rng, out);
      return;
    }
    case Mode::kStructured: {
      ChaCha rng(seed, kTagStructured, static_cast<uint32_t>(index));
      Out out(p, p + n);
      int64_t ms = kEpochMs + static_cast<int64_t>(index) * 600000;
      LogInto(rng, out, ms);
      return;
    }
    case Mode::kMixed: {
      // Segments of 16..256 KiB: 50% text, 25% structured logs, 25% random.
      ChaCha rng(seed, kTagMixed, static_cast<uint32_t>(index));
      int64_t ms = kEpochMs + static_cast<int64_t>(index) * 600000;
      size_t pos = 0;
      while (pos < n) {
        uint32_t kind = rng.Below(4);
        size_t len = std::min<size_t>(n - pos, (16 << 10) + rng.Below(240 << 10));
        Out out(p + pos, p + pos + len);
        if (kind <= 1) TextInto(rng, out);
        else if (kind == 2) LogInto(rng, out, ms);
        else out.Random(rng, len);
        pos += len;
      }
      return;
    }
  }
}

}  // namespace

std::optional<Mode> ParseMode(std::string_view name) {
  if (name == "random") return Mode::kRandom;
  if (name == "text") return Mode::kText;
  if (name == "structured") return Mode::kStructured;
  if (name == "mixed") return Mode::kMixed;
  return std::nullopt;
}

void Generate(Mode mode, uint64_t seed, uint64_t size, unsigned threads, const Sink& sink) {
  threads = std::max(1u, threads);
  const uint64_t blocks = (size + kBlockSize - 1) / kBlockSize;
  const uint64_t batch = uint64_t{threads} * 4;  // blocks per batch
  std::vector<uint8_t> bufs[2] = {std::vector<uint8_t>(batch * kBlockSize), std::vector<uint8_t>(batch * kBlockSize)};
  std::future<void> writing;
  int cur = 0;
  for (uint64_t first = 0; first < blocks; first += batch) {
    const uint64_t count = std::min(batch, blocks - first);
    uint8_t* base = bufs[cur].data();
    auto len_of = [&](uint64_t b) { return static_cast<size_t>(std::min<uint64_t>(kBlockSize, size - b * kBlockSize)); };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        for (uint64_t i = t; i < count; i += threads) Block(mode, seed, first + i, base + i * kBlockSize, len_of(first + i));
      });
    }
    for (auto& th : pool) th.join();
    const size_t bytes = static_cast<size_t>((count - 1) * kBlockSize + len_of(first + count - 1));
    if (writing.valid()) writing.get();  // previous batch fully written before we hand out this one
    writing = std::async(std::launch::async, [&sink, base, bytes] { sink(base, bytes); });
    cur ^= 1;
  }
  if (writing.valid()) writing.get();
}

std::vector<uint8_t> GenerateToVector(Mode mode, uint64_t seed, uint64_t size, unsigned threads) {
  std::vector<uint8_t> v;
  v.reserve(size);
  Generate(mode, seed, size, threads, [&v](const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); });
  return v;
}

MutateStats Mutate(uint64_t seed, uint32_t rate_ppm, const Source& in, const Sink& out) {
  MutateStats st;
  ChaCha rng(seed, kTagMutate, 0);
  // Mean edit length is (1 + kMaxEditLen) / 2; mean gap G = mean_len / rate. Gap ~ U[0, 2G).
  const uint64_t two_g = rate_ppm == 0 ? 0 : (uint64_t{1 + kMaxEditLen} * 1000000) / rate_ppm;
  auto next_gap = [&] { return rate_ppm == 0 ? UINT64_MAX : rng.Below64(two_g); };

  std::vector<uint8_t> ibuf(4 << 20), rbuf(kMaxEditLen);
  uint64_t gap = next_gap();
  uint64_t skip = 0;  // input bytes still to drop (delete / overwrite)
  for (;;) {
    size_t n = in(ibuf.data(), ibuf.size());
    if (n == 0) break;
    st.input_bytes += n;
    size_t pos = 0;
    while (pos < n) {
      if (skip > 0) {
        size_t k = static_cast<size_t>(std::min<uint64_t>(skip, n - pos));
        pos += k;
        skip -= k;
        continue;
      }
      size_t k = static_cast<size_t>(std::min<uint64_t>(gap, n - pos));
      if (k > 0) {
        out(ibuf.data() + pos, k);
        st.output_bytes += k;
        pos += k;
        gap -= k;
      }
      if (gap > 0) continue;  // edit point lies in a later read
      uint32_t op = rng.Below(3);
      uint32_t len = 1 + rng.Below(kMaxEditLen);
      ++st.edits;
      if (op != 1) {  // insert or overwrite: emit new bytes
        rng.Fill(rbuf.data(), len);
        out(rbuf.data(), len);
        st.output_bytes += len;
      }
      if (op == 0) st.inserted += len;
      else if (op == 1) st.deleted += len, skip = len;
      else st.overwritten += len, skip = len;
      gap = next_gap();
    }
  }
  return st;
}

}  // namespace dedup::gen
