#include "cache_file.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

static std::string digest(const std::string &body) {
  return std::to_string(std::hash<std::string>{}(body));
}
static void put(const std::string &path, const std::string &body) {
  std::ofstream file(path, std::ios::binary);
  file << body;
  assert(file.good());
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const auto path = (std::filesystem::path(argv[1]) / "index").string();
  const std::string body(74997, 'i'), old = "previous object remains readable";
  const auto hash = digest(body);
  size_t writes = 0, yielded = 0, bytes = 0;
  put(path, old);
  // A backend that cannot reclaim more than one block per request must still
  // accept the complete 75 KB index through the real publication transaction.
  auto limited_write = [&](const char *data, size_t count, FILE *file) {
    ++writes;
    assert(yielded == writes); // Cooperate before each filesystem write.
    if (count > 4096) {
      errno = ENOSPC;
      return size_t{0};
    }
    bytes += count;
    return std::fwrite(data, 1, count, file);
  };
  assert(rd::publish_cache_file(path, body, hash, digest, limited_write,
                                 [&] { ++yielded; }));
  assert(rd::read_cache_file(path, body.size()) == body);
  assert(writes == (body.size() + rd::cache_write_chunk - 1) /
                       rd::cache_write_chunk);
  assert(bytes == body.size() && !std::filesystem::exists(path + ".part"));
  // Reusing a valid immutable object performs no writes or extra yields.
  writes = yielded = 0;
  assert(rd::publish_cache_file(path, body, hash, digest, limited_write,
                                 [&] { ++yielded; }));
  assert(writes == 0 && yielded == 0);

  for (int fault = 0; fault < 3; ++fault) {
    put(path, old);
    writes = yielded = 0;
    const auto failing_write = [&](const char *data, size_t count, FILE *file) {
      ++writes;
      assert(count <= 4096 && yielded == writes);
      if (writes == 2) {
        if (fault == 0) {
          const auto partial = count / 2;
          assert(std::fwrite(data, 1, partial, file) == partial);
          errno = ENOSPC;
          return partial;
        }
        if (fault == 1) {
          errno = EIO;
          return size_t{0};
        }
        // A short write may omit errno; callers still receive an I/O error.
        return size_t{0};
      }
      return std::fwrite(data, 1, count, file);
    };
    assert(!rd::publish_cache_file(path, body, hash, digest, failing_write,
                                    [&] { ++yielded; }));
    assert(errno == (fault == 0 ? ENOSPC : EIO));
    assert(writes == 2 && yielded == 2);
    assert(rd::read_cache_file(path, old.size()) == old);
    assert(!std::filesystem::exists(path + ".part"));
  }
  // A backend claiming a complete write cannot publish corrupted stage bytes.
  put(path, old);
  assert(!rd::publish_cache_file(
      path, body, hash, digest,
      [](const char *, size_t count, FILE *file) {
        const std::string corrupt(count, 'x');
        return std::fwrite(corrupt.data(), 1, count, file);
      },
      [] {}));
  assert(errno == EIO && rd::read_cache_file(path, old.size()) == old);
  assert(!std::filesystem::exists(path + ".part"));
  // Digest mismatch is rejected before opening the stage or touching old data.
  assert(!rd::publish_cache_file(path, "invalid", hash, digest));
  assert(rd::read_cache_file(path, old.size()) == old);
  // The ordinary firmware overload also publishes a large complete object.
  assert(rd::publish_cache_file(path, body, hash, digest));
  assert(rd::read_cache_file(path, body.size()) == body);
}
