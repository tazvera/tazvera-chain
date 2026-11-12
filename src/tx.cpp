#include "taz/tx.hpp"

namespace taz {

std::vector<uint8_t> Transaction::signable_bytes() const {
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), from.begin(), from.end());
    buf.insert(buf.end(), to.begin(), to.end());
    
    auto append_u64 = [&](uint64_t val) {
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xff));
        }
    };

    append_u64(amount);
    append_u64(fee);
    append_u64(nonce);
    append_u64(expiry_height);
    return buf;
}

std::optional<std::string> validate_tx_shape(const Transaction& tx) {
    if (tx.amount == 0) return "amount must be greater than zero";
    return std::nullopt;
}

}
