#include "taz/block.hpp"
#include "taz/crypto.hpp"

namespace taz {

Hash32 Block::hash() const {
    std::vector<uint8_t> buf;
    auto append_u64 = [&](uint64_t val) {
        for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xff));
    };
    append_u64(height);
    buf.insert(buf.end(), prev_hash.begin(), prev_hash.end());
    append_u64(timestamp);
    return sha256(buf);
}

}
