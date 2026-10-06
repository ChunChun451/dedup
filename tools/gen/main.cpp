// dedup-gen: deterministic test data for the dedup benchmarks.
//   dedup-gen random|text|structured|mixed --seed N --size SIZE [--threads T] [-o FILE]
//   dedup-gen mutate --seed N --rate R --in FILE [-o FILE]        (statistics as JSON on stderr)
// SIZE accepts K, M, G, T suffixes in binary units (4G = 4 GiB). Output goes to stdout unless -o is given.
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "gen.hpp"

namespace {

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "dedup-gen: %s\n", msg.c_str());
  std::exit(2);
}

std::optional<uint64_t> ParseSize(std::string_view s) {
  if (s.empty()) return std::nullopt;
  uint64_t mult = 1;
  switch (s.back()) {
    case 'K': mult = uint64_t{1} << 10; break;
    case 'M': mult = uint64_t{1} << 20; break;
    case 'G': mult = uint64_t{1} << 30; break;
    case 'T': mult = uint64_t{1} << 40; break;
    default: break;
  }
  if (mult != 1) s.remove_suffix(1);
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  return v * mult;
}

void WriteAll(int fd, const uint8_t* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      Die(std::string("write failed: ") + std::strerror(errno));
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
}

int OpenOut(const std::string& path) {
  if (path.empty() || path == "-") return STDOUT_FILENO;
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) Die("cannot create " + path + ": " + std::strerror(errno));
  return fd;
}

void Usage() {
  std::fprintf(stderr,
               "usage: dedup-gen random|text|structured|mixed --seed N --size SIZE [--threads T] [-o FILE]\n"
               "       dedup-gen mutate --seed N --rate R --in FILE [-o FILE]\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) Usage();
  const std::string_view cmd = argv[1];
  std::optional<uint64_t> seed, size;
  std::optional<double> rate;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::string out_path, in_path;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    auto val = [&]() -> std::string_view {
      if (i + 1 >= argc) Die(std::string(a) + " needs a value");
      return argv[++i];
    };
    if (a == "--seed") seed = ParseSize(val());
    else if (a == "--size") { size = ParseSize(val()); if (!size) Die("bad --size"); }
    else if (a == "--threads") threads = static_cast<unsigned>(std::atoi(std::string(val()).c_str()));
    else if (a == "-o") out_path = val();
    else if (a == "--in") in_path = val();
    else if (a == "--rate") rate = std::atof(std::string(val()).c_str());
    else Usage();
  }
  if (!seed) Die("--seed is required");

  if (cmd == "mutate") {
    if (!rate || *rate < 0 || *rate > 0.5) Die("--rate must be in [0, 0.5]");
    if (in_path.empty()) Die("--in is required");
    int in = ::open(in_path.c_str(), O_RDONLY);
    if (in < 0) Die("cannot open " + in_path + ": " + std::strerror(errno));
    int out = OpenOut(out_path);
    const auto ppm = static_cast<uint32_t>(std::llround(*rate * 1e6));
    auto st = dedup::gen::Mutate(
        *seed, ppm,
        [in](uint8_t* p, size_t cap) -> size_t {
          for (;;) {
            ssize_t r = ::read(in, p, cap);
            if (r >= 0) return static_cast<size_t>(r);
            if (errno != EINTR) Die(std::string("read failed: ") + std::strerror(errno));
          }
        },
        [out](const uint8_t* p, size_t n) { WriteAll(out, p, n); });
    if (out != STDOUT_FILENO && ::close(out) != 0) Die("close failed");
    std::fprintf(stderr,
                 "{\"input_bytes\": %llu, \"output_bytes\": %llu, \"edits\": %llu, \"inserted\": %llu, "
                 "\"deleted\": %llu, \"overwritten\": %llu, \"edit_fraction\": %.6f}\n",
                 (unsigned long long)st.input_bytes, (unsigned long long)st.output_bytes,
                 (unsigned long long)st.edits, (unsigned long long)st.inserted, (unsigned long long)st.deleted,
                 (unsigned long long)st.overwritten, st.EditFraction());
    return 0;
  }

  auto mode = dedup::gen::ParseMode(cmd);
  if (!mode) Usage();
  if (!size) Die("--size is required");
  int out = OpenOut(out_path);
  dedup::gen::Generate(*mode, *seed, *size, threads, [out](const uint8_t* p, size_t n) { WriteAll(out, p, n); });
  if (out != STDOUT_FILENO && ::close(out) != 0) Die("close failed");
  return 0;
}
