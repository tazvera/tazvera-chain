#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace taz {
using Amount = std::uint64_t;
using Hash32 = std::array<std::uint8_t, 32>;
using Address = std::array<std::uint8_t, 20>;
using PubKey = std::array<std::uint8_t, 32>;
using PrivKey = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;

// Core v8.2: protocol 2 = R2 veto (one-shot postpone). Protocol 1 chain files are not loadable in this build.
// v8.9: the one place the core version lives. CMakeLists.txt reads this line and the test suite compares
// it with the README title, so the three can no longer drift apart (they had drifted by six releases).
inline constexpr std::string_view CORE_VERSION = "9.1";
inline constexpr std::uint32_t PROTOCOL_VERSION = 2;
inline constexpr std::string_view CHAIN_ID = "tazvera-local-prototype-v1";
inline constexpr Amount FEE_TRANSFER_V1 = 1000;
inline constexpr std::size_t MAX_BLOCK_TXS = 10000;
inline constexpr std::uint64_t LIMIT_WINDOW_BLOCKS = 1000;
inline constexpr std::uint64_t MAX_HOLD_BLOCKS = 100000;
inline constexpr std::uint64_t SAFETY_DELAY_BLOCKS = 500;
inline constexpr std::size_t MAX_GUARDIANS = 5;
inline constexpr std::uint64_t RECOVERY_DELAY_BLOCKS = 300;
// After a veto postpones once, recovery still executes; a thief cannot block forever.
inline constexpr std::uint64_t VETO_POSTPONE_BLOCKS = 300;
}
