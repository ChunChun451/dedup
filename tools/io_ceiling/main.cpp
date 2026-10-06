// io_ceiling: the fastest this disk can read a dataset and write a given number of bytes (SPEC.md 4.2, G1b).
// It is the reference time for P4a and P5b: a backup cannot beat reading its input plus writing its repo.
//
//   io_ceiling --read PATH [--read PATH...] --write-bytes SIZE [--dir DIR] [--threads N] [--json]
//
// Reads every regular file under the PATHs (sorted path order) with O_DIRECT, split into segments of up to 16 MiB
// that N threads read in 1 MiB requests. Writes SIZE bytes to a new file in DIR with O_DIRECT from N threads, then
// fdatasync (space reserved first with fallocate). Two modes, each after dropping the page cache (if the sudo
// helper is installed):
//   sequential: read everything, then write   -> total = read time + write time
//   overlapped: read and write at the same time -> total = until both finish
// ceiling = the faster mode. SIZE accepts K, M, G suffixes (binary units).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {

constexpr size_t kIo = size_t{1} << 20;
constexpr uint64_t kSegment = uint64_t{16} << 20;
constexpr size_t kAlign = 4096;

[[noreturn]] void Die(const std::string& m) {
  std::fprintf(stderr, "io_ceiling: %s\n", m.c_str());
  std::exit(2);
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Segment {
  size_t file;
  uint64_t offset, len;
};

struct Input {
  std::vector<std::string> files;
  std::vector<Segment> segments;
  uint64_t bytes = 0;
};

Input Collect(const std::vector<std::string>& roots) {
  Input in;
  for (const auto& r : roots) {
    fs::path root(r);
    if (fs::is_regular_file(fs::symlink_status(root))) {
      in.files.push_back(root.string());
      continue;
    }
    if (!fs::is_directory(root)) Die("not a file or directory: " + r);
    std::vector<std::string> found;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      std::error_code ec;
      if (e.is_regular_file(ec) && !e.is_symlink(ec)) found.push_back(e.path().string());
    }
    std::sort(found.begin(), found.end());
    in.files.insert(in.files.end(), found.begin(), found.end());
  }
  for (size_t i = 0; i < in.files.size(); ++i) {
    struct stat st;
    if (::stat(in.files[i].c_str(), &st) != 0) Die("stat " + in.files[i]);
    uint64_t size = static_cast<uint64_t>(st.st_size);
    in.bytes += size;
    for (uint64_t off = 0; off < size || (size == 0 && off == 0); off += kSegment) {
      in.segments.push_back({i, off, std::min(kSegment, size - off)});
      if (size == 0) break;
    }
  }
  return in;
}

struct Buf {
  explicit Buf(size_t n) : p(static_cast<uint8_t*>(std::aligned_alloc(kAlign, n))) {
    if (!p) Die("out of memory");
  }
  ~Buf() { std::free(p); }
  uint8_t* p;
};

// Read all segments with `threads` threads; returns seconds.
double ReadAll(const Input& in, unsigned threads) {
  std::atomic<size_t> next{0};
  double t0 = Now();
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      Buf buf(kIo);
      for (size_t i; (i = next.fetch_add(1)) < in.segments.size();) {
        const Segment& s = in.segments[i];
        int fd = ::open(in.files[s.file].c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0) fd = ::open(in.files[s.file].c_str(), O_RDONLY);  // e.g. filesystems without O_DIRECT
        if (fd < 0) Die("open " + in.files[s.file] + ": " + std::strerror(errno));
        for (uint64_t done = 0; done < s.len;) {
          ssize_t r = ::pread(fd, buf.p, kIo, static_cast<off_t>(s.offset + done));
          if (r < 0) Die("read " + in.files[s.file] + ": " + std::strerror(errno));
          if (r == 0) break;
          done += static_cast<uint64_t>(r);
        }
        ::close(fd);
      }
    });
  }
  for (auto& th : pool) th.join();
  return Now() - t0;
}

// Write `bytes` (rounded up to 1 MiB) to a new file with `threads` threads, then fdatasync; returns seconds.
double WriteAll(const std::string& path, uint64_t bytes, unsigned threads) {
  const uint64_t blocks = (bytes + kIo - 1) / kIo;
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
  if (fd < 0) Die("create " + path + ": " + std::strerror(errno));
  std::atomic<uint64_t> next{0};
  double t0 = Now();
  // Reserve the space first (as fio does). Without it, 8 threads extending a new file with O_DIRECT measured
  // 0.6-0.75 GB/s instead of ~1.8, because ext4 allocates blocks during each write. Timed: a tool could do the same.
  if (int e = ::posix_fallocate(fd, 0, static_cast<off_t>(blocks * kIo)); e != 0) {
    Die("fallocate " + path + ": " + std::strerror(e));
  }
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      Buf buf(kIo);
      for (size_t i = 0; i < kIo; ++i) buf.p[i] = static_cast<uint8_t>(i * 131 + t * 7 + 1);  // not zeros
      for (uint64_t b; (b = next.fetch_add(1)) < blocks;) {
        if (::pwrite(fd, buf.p, kIo, static_cast<off_t>(b * kIo)) != static_cast<ssize_t>(kIo)) {
          Die("write " + path + ": " + std::strerror(errno));
        }
      }
    });
  }
  for (auto& th : pool) th.join();
  if (::fdatasync(fd) != 0) Die("fdatasync failed");
  double s = Now() - t0;
  ::close(fd);
  ::unlink(path.c_str());
  return s;
}

bool DropCaches() {
  return std::system("sudo -n /usr/local/sbin/dedup-drop-caches >/dev/null 2>&1") == 0;
}

uint64_t ParseSize(const std::string& s) {
  if (s.empty()) Die("empty size");
  uint64_t mult = 1;
  std::string num = s;
  switch (s.back()) {
    case 'K': mult = uint64_t{1} << 10; num.pop_back(); break;
    case 'M': mult = uint64_t{1} << 20; num.pop_back(); break;
    case 'G': mult = uint64_t{1} << 30; num.pop_back(); break;
    default: break;
  }
  char* end = nullptr;
  uint64_t v = std::strtoull(num.c_str(), &end, 10);
  if (end == num.c_str() || *end) Die("bad size: " + s);
  return v * mult;
}

struct Mode {
  double read_s = 0, write_s = 0, total_s = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> reads;
  uint64_t write_bytes = 0;
  std::string dir;
  unsigned threads = 8;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) Die(a + " needs a value");
      return argv[++i];
    };
    if (a == "--read") reads.push_back(val());
    else if (a == "--write-bytes") write_bytes = ParseSize(val());
    else if (a == "--dir") dir = val();
    else if (a == "--threads") threads = static_cast<unsigned>(std::max(1, std::atoi(val().c_str())));
    else if (a == "--json") json = true;
    else Die("usage: io_ceiling --read PATH [--read PATH...] --write-bytes SIZE [--dir DIR] [--threads N] [--json]");
  }
  if (reads.empty()) Die("--read is required");
  if (dir.empty()) {
    // Default: data/scratch next to the first input, so the write goes to the same disk.
    fs::path p = fs::absolute(reads[0]);
    while (p.has_parent_path() && p.filename() != "data") p = p.parent_path();
    dir = (p.filename() == "data" ? p / "scratch/io_ceiling" : fs::path(reads[0]).parent_path()).string();
  }
  fs::create_directories(dir);
  const std::string wpath = (fs::path(dir) / ("io_ceiling." + std::to_string(::getpid()) + ".tmp")).string();
  Input in = Collect(reads);

  bool dropped = DropCaches();
  Mode seq;
  seq.read_s = ReadAll(in, threads);
  seq.write_s = write_bytes ? WriteAll(wpath, write_bytes, threads) : 0.0;
  seq.total_s = seq.read_s + seq.write_s;

  dropped = DropCaches() && dropped;
  Mode ovl;
  double t0 = Now();
  std::thread writer([&] { ovl.write_s = write_bytes ? WriteAll(wpath, write_bytes, threads) : 0.0; });
  ovl.read_s = ReadAll(in, threads);
  writer.join();
  ovl.total_s = Now() - t0;

  const bool overlapped_wins = ovl.total_s < seq.total_s;
  const double ceiling = overlapped_wins ? ovl.total_s : seq.total_s;
  if (json) {
    auto mode = [&](const Mode& m) {
      char b[256];
      std::snprintf(b, sizeof b,
                    "{\"read_s\": %.4f, \"write_s\": %.4f, \"total_s\": %.4f, \"read_gbps\": %.3f, \"write_gbps\": %.3f}",
                    m.read_s, m.write_s, m.total_s, in.bytes / m.read_s / 1e9,
                    write_bytes ? write_bytes / m.write_s / 1e9 : 0.0);
      return std::string(b);
    };
    std::printf("{\"read_bytes\": %llu, \"files\": %zu, \"write_bytes\": %llu, \"threads\": %u, "
                "\"caches_dropped\": %s, \"sequential\": %s, \"overlapped\": %s, \"ceiling_s\": %.4f, "
                "\"ceiling_mode\": \"%s\", \"ceiling_read_gbps\": %.3f}\n",
                (unsigned long long)in.bytes, in.files.size(), (unsigned long long)write_bytes, threads,
                dropped ? "true" : "false", mode(seq).c_str(), mode(ovl).c_str(), ceiling,
                overlapped_wins ? "overlapped" : "sequential", in.bytes / ceiling / 1e9);
  } else {
    std::printf("input %.2f GB in %zu files, write %.2f GB, %u threads%s\n", in.bytes / 1e9, in.files.size(),
                write_bytes / 1e9, threads, dropped ? "" : " (page cache NOT dropped)");
    std::printf("sequential: read %.2f s + write %.2f s = %.2f s\n", seq.read_s, seq.write_s, seq.total_s);
    std::printf("overlapped: read %.2f s, write %.2f s, together %.2f s\n", ovl.read_s, ovl.write_s, ovl.total_s);
    std::printf("ceiling: %.2f s (%s) = %.2f GB/s of input\n", ceiling, overlapped_wins ? "overlapped" : "sequential",
                in.bytes / ceiling / 1e9);
  }
  return 0;
}
