#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace taz {

// 1 TZV = 1_000_000 utzv (6 decimal places). All money math is integer.
using Amount = uint64_t;
constexpr Amount UTZV_PER_TZV = 1'000'000ULL;
constexpr Amount MAX_SUPPLY_TZV = 1'000'000'000ULL; // 100 crore TZV
constexpr Amount MAX_SUPPLY_UTZV = MAX_SUPPLY_TZV * UTZV_PER_TZV;

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr uint32_t CHAIN_ID = 1; // prototype local chain

using Hash32 = std::array<uint8_t, 32>;
using PubKey = std::array<uint8_t, 32>;   // Ed25519
using PrivKey = std::array<uint8_t, 32>;  // Ed25519 seed/private
using Signature = std::array<uint8_t, 64>;
using Address = std::array<uint8_t, 20>;  // first 20 bytes of SHA-256(pubkey)

inline Amount tzv_to_utzv(uint64_t whole, uint32_t micros = 0) {
  return whole * UTZV_PER_TZV + micros;
}

std::string to_hex(const uint8_t* data, size_t n);
std::string addr_to_string(const Address& a);
bool parse_hex(const std::string& hex, uint8_t* out, size_t n);

}  // namespace taz
