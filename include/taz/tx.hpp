#pragma once
#include "taz/types.hpp"
#include <optional>

namespace taz {

struct Transaction {
  uint32_t chain_id = CHAIN_ID;
  uint32_t protocol_version = PROTOCOL_VERSION;
  Address from{};
  Address to{};
  Amount amount = 0;       // utzv transferred to receiver
  Amount fee = 0;          // utzv burned on success
  uint64_t nonce = 0;
  uint32_t fee_schedule_version = 1;
  uint64_t expiry_height = 0;  // 0 = no expiry in prototype
  PubKey pubkey{};
  Signature signature{};

  std::vector<uint8_t> signable_bytes() const;
  Hash32 txid() const;
};

// Fixed fee schedule v1 (prototype values — retune after benchmarks).
constexpr Amount FEE_TRANSFER_V1 = 1'000;  // 0.001 TZV

std::optional<std::string> validate_tx_shape(const Transaction& tx);

}  // namespace taz
