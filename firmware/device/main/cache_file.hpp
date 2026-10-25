#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace rd {
inline constexpr size_t cache_read_chunk = 4096;
inline constexpr size_t cache_write_chunk = 4096;
inline void cache_read_yield() {
#ifdef ESP_PLATFORM
  // A yield to another ready task does not let IDLE run. Block for one tick
  // between filesystem operations so IDLE can service its watchdog subscription.
  vTaskDelay(1);
#endif
}

template <typename Yield>
std::string read_cache_file(const std::string &path, size_t limit,
                            Yield cooperate) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return "";
  // Seeking to EOF makes SPIFFS locate the last object-index span and then
  // locate the first span again on rewind. fstat reads the open descriptor's
  // size without either full lookup.
  struct stat info{};
  if (fstat(fileno(f), &info) != 0 || info.st_size < 0 ||
      static_cast<size_t>(info.st_size) > limit) {
    std::fclose(f);
    return "";
  }
  const size_t size = static_cast<size_t>(info.st_size);
  std::string out(size, '\0');
  size_t got = 0;
  while (got < size) {
    cooperate();
    const size_t count = std::min(cache_read_chunk, size - got);
    if (std::fread(out.data() + got, 1, count, f) != count) {
      std::fclose(f);
      return ""; // Never publish or validate an incomplete cache read.
    }
    got += count;
  }
  std::fclose(f);
  return out;
}

inline std::string read_cache_file(const std::string &path, size_t limit) {
  return read_cache_file(path, limit, cache_read_yield);
}

template <typename Digest, typename Write, typename Yield>
bool publish_cache_file(const std::string &path, const std::string &body,
                        const std::string &digest, Digest hash, Write write,
                        Yield cooperate) {
  if (hash(body) != digest)
    return false;
  if (hash(read_cache_file(path, body.size())) == digest)
    return true;
  const std::string temp = path + ".part";
  FILE *f = std::fopen(temp.c_str(), "wb");
  if (!f)
    return false;
  // SPIFFS must reclaim enough pages for each underlying write before it
  // starts. A large write can exceed its per-call GC budget despite ample
  // deleted space. Disable stdio buffering so each request stays bounded.
  errno = 0;
  bool ok = std::setvbuf(f, nullptr, _IONBF, 0) == 0;
  int failure_errno = ok ? 0 : (errno ? errno : EIO);
  size_t written = 0;
  while (ok && written < body.size()) {
    cooperate();
    const size_t count = std::min(cache_write_chunk, body.size() - written);
    errno = 0;
    if (write(body.data() + written, count, f) != count) {
      failure_errno = errno ? errno : EIO;
      ok = false;
      break;
    }
    written += count;
  }
  if (ok && std::fflush(f) != 0) {
    failure_errno = errno ? errno : EIO;
    ok = false;
  }
  if (ok && fsync(fileno(f)) != 0) {
    failure_errno = errno ? errno : EIO;
    ok = false;
  }
  if (std::fclose(f) != 0) {
    if (ok)
      failure_errno = errno ? errno : EIO;
    ok = false;
  }
  if (ok) {
    errno = 0;
    ok = hash(read_cache_file(temp, body.size())) == digest;
    if (!ok)
      failure_errno = errno ? errno : EIO;
  }
  // SPIFFS cannot rename over an existing object. Only a verified-bad target
  // may be removed, and only after its replacement is durable and verified.
  // An interruption leaves a valid old object or a recoverable missing object.
  if (ok && hash(read_cache_file(path, body.size())) != digest &&
      std::remove(path.c_str()) != 0 && errno != ENOENT) {
    failure_errno = errno;
    ok = false;
  }
  if (ok) {
    ok = std::rename(temp.c_str(), path.c_str()) == 0;
    if (!ok)
      failure_errno = errno;
  }
  if (!ok) {
    std::remove(temp.c_str());
    errno = failure_errno;
  }
  return ok;
}

template <typename Digest>
bool publish_cache_file(const std::string &path, const std::string &body,
                        const std::string &digest, Digest hash) {
  return publish_cache_file(
      path, body, digest, hash,
      [](const char *data, size_t count, FILE *file) {
        return std::fwrite(data, 1, count, file);
      },
      cache_read_yield);
}
} // namespace rd
