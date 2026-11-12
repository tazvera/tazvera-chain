#ifndef TAZ_TYPES_HPP
#define TAZ_TYPES_HPP

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace taz {
using Amount = uint64_t;
using Hash32 = std::array<uint8_t, 32>;
using Address = std::array<uint8_t, 20>;
using PubKey = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
}

#endif
