#include "taz/json_io.hpp"
#include "taz/crypto.hpp"
#include "taz/types.hpp"
#include <cctype>
#include <cstring>
#include <sstream>

namespace taz {

std::string hash_to_hex(const Hash32& h) { return to_hex(h.data(), h.size()); }

bool hash_from_hex(const std::string& hex, Hash32& out) {
  return parse_hex(hex, out.data(), out.size());
}

static std::string hex_of(const uint8_t* p, size_t n) { return to_hex(p, n); }

static void skip_ws(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

static bool expect(const std::string& s, size_t& i, char c) {
  skip_ws(s, i);
  if (i >= s.size() || s[i] != c) return false;
  ++i;
  return true;
}

static bool parse_string(const std::string& s, size_t& i, std::string& out) {
  skip_ws(s, i);
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  out.clear();
  while (i < s.size() && s[i] != '"') {
    if (s[i] == '\\') return false;  // no escapes in our format
    out.push_back(s[i++]);
  }
  if (i >= s.size()) return false;
  ++i;
  return true;
}

static bool parse_u64(const std::string& s, size_t& i, uint64_t& out) {
  skip_ws(s, i);
  if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  uint64_t v = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
    ++i;
  }
  out = v;
  return true;
}

static bool parse_u32(const std::string& s, size_t& i, uint32_t& out) {
  uint64_t v = 0;
  if (!parse_u64(s, i, v) || v > UINT32_MAX) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// Minimal object field reader: finds "key": value patterns in order (our writers are ordered).
static bool find_key(const std::string& s, size_t& i, const char* key) {
  skip_ws(s, i);
  std::string k;
  if (!expect(s, i, '"')) return false;
  --i;  // parse_string expects starting quote
  if (!parse_string(s, i, k) || k != key) return false;
  if (!expect(s, i, ':')) return false;
  return true;
}

std::string tx_to_json(const Transaction& tx) {
  std::ostringstream o;
  o << '{'
    << "\"chain_id\":" << tx.chain_id << ','
    << "\"protocol_version\":" << tx.protocol_version << ','
    << "\"from\":\"" << hex_of(tx.from.data(), tx.from.size()) << "\","
    << "\"to\":\"" << hex_of(tx.to.data(), tx.to.size()) << "\","
    << "\"amount\":" << tx.amount << ','
    << "\"fee\":" << tx.fee << ','
    << "\"nonce\":" << tx.nonce << ','
    << "\"fee_schedule_version\":" << tx.fee_schedule_version << ','
    << "\"expiry_height\":" << tx.expiry_height << ','
    << "\"pubkey\":\"" << hex_of(tx.pubkey.data(), tx.pubkey.size()) << "\","
    << "\"signature\":\"" << hex_of(tx.signature.data(), tx.signature.size()) << "\""
    << '}';
  return o.str();
}

std::optional<Transaction> tx_from_json(const std::string& json) {
  Transaction tx;
  size_t i = 0;
  if (!expect(json, i, '{')) return std::nullopt;
  if (!find_key(json, i, "chain_id") || !parse_u32(json, i, tx.chain_id)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "protocol_version") || !parse_u32(json, i, tx.protocol_version)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;

  std::string hex;
  if (!find_key(json, i, "from") || !parse_string(json, i, hex) ||
      !parse_hex(hex, tx.from.data(), tx.from.size()))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "to") || !parse_string(json, i, hex) ||
      !parse_hex(hex, tx.to.data(), tx.to.size()))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "amount") || !parse_u64(json, i, tx.amount)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "fee") || !parse_u64(json, i, tx.fee)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "nonce") || !parse_u64(json, i, tx.nonce)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "fee_schedule_version") || !parse_u32(json, i, tx.fee_schedule_version))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "expiry_height") || !parse_u64(json, i, tx.expiry_height)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "pubkey") || !parse_string(json, i, hex) ||
      !parse_hex(hex, tx.pubkey.data(), tx.pubkey.size()))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "signature") || !parse_string(json, i, hex) ||
      !parse_hex(hex, tx.signature.data(), tx.signature.size()))
    return std::nullopt;
  if (!expect(json, i, '}')) return std::nullopt;
  return tx;
}

std::string block_to_json(const Block& block) {
  std::ostringstream o;
  const auto& h = block.header;
  o << '{'
    << "\"protocol_version\":" << h.protocol_version << ','
    << "\"chain_id\":" << h.chain_id << ','
    << "\"height\":" << h.height << ','
    << "\"prev_hash\":\"" << hash_to_hex(h.prev_hash) << "\","
    << "\"timestamp\":" << h.timestamp << ','
    << "\"tx_root\":\"" << hash_to_hex(h.tx_root) << "\","
    << "\"state_root\":\"" << hash_to_hex(h.state_root) << "\","
    << "\"transactions\":[";
  for (size_t t = 0; t < block.txs.size(); ++t) {
    if (t) o << ',';
    o << tx_to_json(block.txs[t]);
  }
  o << "]}";
  return o.str();
}

std::optional<Block> block_from_json(const std::string& json) {
  Block b;
  size_t i = 0;
  if (!expect(json, i, '{')) return std::nullopt;
  if (!find_key(json, i, "protocol_version") || !parse_u32(json, i, b.header.protocol_version))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "chain_id") || !parse_u32(json, i, b.header.chain_id)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "height") || !parse_u64(json, i, b.header.height)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  std::string hex;
  if (!find_key(json, i, "prev_hash") || !parse_string(json, i, hex) || !hash_from_hex(hex, b.header.prev_hash))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "timestamp") || !parse_u64(json, i, b.header.timestamp)) return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "tx_root") || !parse_string(json, i, hex) || !hash_from_hex(hex, b.header.tx_root))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "state_root") || !parse_string(json, i, hex) ||
      !hash_from_hex(hex, b.header.state_root))
    return std::nullopt;
  if (!expect(json, i, ',')) return std::nullopt;
  if (!find_key(json, i, "transactions")) return std::nullopt;
  if (!expect(json, i, '[')) return std::nullopt;
  skip_ws(json, i);
  if (i < json.size() && json[i] == ']') {
    ++i;
  } else {
    while (true) {
      // Parse one tx object by finding matching braces
      skip_ws(json, i);
      if (i >= json.size() || json[i] != '{') return std::nullopt;
      size_t start = i;
      int depth = 0;
      do {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') --depth;
        ++i;
      } while (i < json.size() && depth > 0);
      if (depth != 0) return std::nullopt;
      auto tx = tx_from_json(json.substr(start, i - start));
      if (!tx) return std::nullopt;
      b.txs.push_back(*tx);
      skip_ws(json, i);
      if (i < json.size() && json[i] == ',') {
        ++i;
        continue;
      }
      if (i < json.size() && json[i] == ']') {
        ++i;
        break;
      }
      return std::nullopt;
    }
  }
  if (!expect(json, i, '}')) return std::nullopt;
  return b;
}

}  // namespace taz
