#include "taz/state.hpp"
#include "taz/crypto.hpp"
#include <sstream>

namespace taz {

void State::set_balance(const Address& a, Amount bal) {
  accounts_[a].balance = bal;
}

Amount State::balance(const Address& a) const {
  auto it = accounts_.find(a);
  return it == accounts_.end() ? 0 : it->second.balance;
}

uint64_t State::nonce(const Address& a) const {
  auto it = accounts_.find(a);
  return it == accounts_.end() ? 0 : it->second.nonce;
}

Amount State::circulating() const {
  Amount s = 0;
  for (const auto& [_, acc] : accounts_) {
    if (s > UINT64_MAX - acc.balance) return UINT64_MAX;
    s += acc.balance;
  }
  return s;
}

ApplyResult State::apply(const Transaction& tx, uint64_t current_height) {
  ApplyResult r;

  if (auto err = validate_tx_shape(tx)) {
    r.error = *err;
    return r;
  }

  if (tx.expiry_height != 0 && current_height > tx.expiry_height) {
    r.error = "transaction expired";
    return r;
  }

  auto msg = tx.signable_bytes();
  if (!verify(tx.pubkey, msg.data(), msg.size(), tx.signature)) {
    r.error = "invalid signature";
    return r;
  }

  Account& sender = accounts_[tx.from];
  if (tx.nonce != sender.nonce) {
    r.error = "bad nonce (possible double-spend or replay)";
    return r;
  }

  Amount need = tx.amount + tx.fee;
  if (sender.balance < need) {
    r.error = "insufficient balance";
    return r;
  }

  sender.balance -= need;
  sender.nonce += 1;
  accounts_[tx.to].balance += tx.amount;
  burned_ += tx.fee;
  r.ok = true;
  r.burned = tx.fee;
  return r;
}

Hash32 State::state_root() const {
  // Simple deterministic fingerprint: hash of sorted (addr||balance||nonce).
  std::vector<uint8_t> buf;
  for (const auto& [addr, acc] : accounts_) {
    buf.insert(buf.end(), addr.begin(), addr.end());
    for (int i = 7; i >= 0; --i)
      buf.push_back(static_cast<uint8_t>((acc.balance >> (i * 8)) & 0xff));
    for (int i = 7; i >= 0; --i)
      buf.push_back(static_cast<uint8_t>((acc.nonce >> (i * 8)) & 0xff));
  }
  for (int i = 7; i >= 0; --i)
    buf.push_back(static_cast<uint8_t>((burned_ >> (i * 8)) & 0xff));
  return sha256(buf);
}

}  // namespace taz
