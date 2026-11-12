#include "taz/types.hpp"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace taz {

std::string to_hex(const uint8_t* data, size_t n) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (size_t i = 0; i < n; ++i) os << std::setw(2) << static_cast<int>(data[i]);
  return os.str();
}

std::string addr_to_string(const Address& a) {
  return "tzv1" + to_hex(a.data(), a.size());
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_hex(const std::string& hex, uint8_t* out, size_t n) {
  if (hex.size() != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    int hi = hex_val(hex[i * 2]);
    int lo = hex_val(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace taz
