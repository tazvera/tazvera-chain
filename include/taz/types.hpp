#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace taz {
using Amount = std::uint64_t; // smallest unit: utzv
using Hash32 = std::array<std::uint8_t, 32>;
using Address = std::array<std::uint8_t, 20>;
using PubKey = std::array<std::uint8_t, 32>;       // Ed25519 public key
using PrivKey = std::array<std::uint8_t, 32>;      // Ed25519 private seed; keep secret
using Signature = std::array<std::uint8_t, 64>;
inline constexpr std::uint32_t PROTOCOL_VERSION = 1;
inline constexpr std::string_view CHAIN_ID = "tazvera-local-prototype-v1";
inline constexpr Amount FEE_TRANSFER_V1 = 1000; // prototype value only; not a mainnet fee
}
