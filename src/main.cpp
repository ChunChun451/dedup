#include <cstdio>
#include <string_view>

#include "version.hpp"

int main(int argc, char** argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--version" || std::string_view(argv[1]) == "-V")) {
    std::printf("dedup %s\n", dedup::kVersion);
    return 0;
  }
  std::fprintf(stderr, "usage: dedup --version\n");
  return 2;
}
