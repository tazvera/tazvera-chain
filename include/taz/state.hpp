#ifndef TAZ_STATE_HPP
#define TAZ_STATE_HPP

#include "taz/types.hpp"
#include "taz/tx.hpp"
#include <unordered_map>

namespace taz {

struct Account {
    Amount balance{0};
    uint64_t nonce{0};
};

struct ApplyResult {
    bool ok{false};
    std::string error;
    Amount burned{0};
};

class State {
public:
    void set_balance(const Address& a, Amount bal);
    Amount balance(const Address& a) const;
    uint64_t nonce(const Address& a) const;
    Amount circulating() const;

    ApplyResult apply(const Transaction& tx, uint64_t current_height);
    Hash32 state_root() const;

private:
    std::unordered_map<Address, Account> accounts_;
    Amount burned_{0};
};

}

#endif
