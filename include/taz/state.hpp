#pragma once
#include "taz/tx.hpp"
#include <map>

namespace taz {
struct Account { Amount balance{0}; std::uint64_t nonce{0}; };
struct ApplyResult { bool ok{false}; std::string error; Amount burned{0}; };
class State {
public:
    void set_balance(const Address& address, Amount amount);
    Amount balance(const Address& address) const;
    std::uint64_t nonce(const Address& address) const;
    Amount burned_total() const noexcept { return burned_; }
    Amount circulating() const;
    ApplyResult apply(const Transaction& tx, std::uint64_t current_height);
    Hash32 state_root() const;
private:
    // std::map gives every node the same canonical address order.
    std::map<Address, Account> accounts_;
    Amount burned_{0};
};
}
