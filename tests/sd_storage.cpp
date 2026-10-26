#include "cache_file.hpp"
#include "external_storage.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
namespace rd {
static std::mutex verified_mutex;
static std::set<std::string> verified_files;
static std::string root;
static uint64_t available = 16000000000ULL;
bool external_cache_enabled() { return true; }
const std::string &cache_directory() { return root; }
StorageCapacity sd_storage_capacity() { return {true, 16000000000ULL, available}; }
static std::string sha256(const std::string &body) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(64) << std::hash<std::string>{}(body);
  return out.str();
}
static bool valid_hash(const std::string &s) {
  return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
#include "sd_storage_under_test.hpp"
}
int main(int argc, char **argv) {
  using namespace rd;
  assert(argc == 2);
  root = argv[1];
  const std::string body = "verified original content";
  const auto id = sha256(body);
  assert(write_file(id, body) && cache_body_verified(id));
  assert(read_file(id, 1024) == body);
  std::ofstream(root + "/" + id) << "damaged content";
  assert(read_file(id, 1024).empty() && !cache_body_verified(id));
  assert(write_file(id, body) && read_file(id, 1024) == body);
  std::filesystem::remove(root + "/" + id);
  assert(read_file(id, 1024).empty() && !cache_body_verified(id));
  available = 7 * 1024 * 1024;
  assert(write_file(id, body)); // Final metadata may consume reserved headroom.
  assert(!write_file("../outside", body));
}
