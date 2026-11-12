#ifndef TAZ_TX_HPP
#define TAZ_TX_HPP

#include "taz/types.hpp"
#include <optional>

namespace taz {

struct Transaction {
    Address from;
    Address to;
    Amount amount;
    Amount fee;
    uint64_t nonce;
    uint64_t expiry_height{0};
    PubKey pubkey;
    Signature signature;

    std::vector<uint8_t> signable_bytes() const;
};

std::optional<std::string> validate_tx_shape(const Transaction& tx);

}

#endif
