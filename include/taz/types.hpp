#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
inline constexpr std::size_t MAX_BLOCK_TXS = 10000; // prototype DoS bound, not a final parameter
// Safety-rule parameters. All are counted in BLOCKS so that every node agrees exactly.
// Prototype values only: real values depend on the block time that is still to be chosen.
inline constexpr std::uint64_t LIMIT_WINDOW_BLOCKS = 1000;    // length of one "day" for the spending limit
inline constexpr std::uint64_t MAX_HOLD_BLOCKS = 100000;      // longest allowed hold on a large transfer
inline constexpr std::uint64_t SAFETY_DELAY_BLOCKS = 500;     // delay before a loosening of the rules takes effect
}
