#include "taz/tx.hpp"
#include "taz/crypto.hpp"
#include <cstring>

namespace taz {

static void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
  b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  b.push_back(static_cast<uint8_t>(v & 0xff));
}

static void put_u64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 7; i >= 0; --i)
    b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}

static void put_bytes(std::vector<uint8_t>& b, const uint8_t* p, size_t n) {
  b.insert(b.end(), p, p + n);
}

std::vector<uint8_t> Transaction::signable_bytes() const {
  // Signature is NOT included. Canonical field order for hashing/signing.
  std::vector<uint8_t> b;
  b.reserve(128);
  put_u32(b, chain_id);
  put_u32(b, protocol_version);
  put_bytes(b, from.data(), from.size());
  put_bytes(b, to.data(), to.size());
  put_u64(b, amount);
  put_u64(b, fee);
  put_u64(b, nonce);
  put_u32(b, fee_schedule_version);
  put_u64(b, expiry_height);
  put_bytes(b, pubkey.data(), pubkey.size());
  return b;
}

Hash32 Transaction::txid() const {
  auto body = signable_bytes();
  put_bytes(body, signature.data(), signature.size());
  return sha256(body);
}

std::optional<std::string> validate_tx_shape(const Transaction& tx) {
  if (tx.chain_id != CHAIN_ID) return "wrong chain id";
  if (tx.protocol_version != PROTOCOL_VERSION) return "unsupported protocol version";
  if (tx.fee_schedule_version != 1) return "unknown fee schedule version";
  if (tx.amount == 0) return "amount must be > 0";
  if (tx.fee != FEE_TRANSFER_V1) return "fee must match schedule v1 transfer fee";
  if (tx.from == tx.to) return "self-transfer not allowed in prototype";
  // overflow: amount + fee
  if (tx.amount > UINT64_MAX - tx.fee) return "amount+fee overflow";
  Address derived = address_from_pubkey(tx.pubkey);
  if (derived != tx.from) return "from address does not match pubkey";
  return std::nullopt;
}

}  // namespace taz
