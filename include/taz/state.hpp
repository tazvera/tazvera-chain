#pragma once
#include "taz/tx.hpp"
#include <map>

namespace taz {
// An account's own protection rules. All zero = no protection (the default).
struct SafetyConfig {
    Amount daily_limit{0};            // max total sent per LIMIT_WINDOW_BLOCKS, 0 = unlimited
    Amount hold_threshold{0};         // a transfer >= this waits before it arrives, 0 = never
    std::uint64_t hold_blocks{0};     // how long it waits
    bool is_default() const noexcept { return daily_limit == 0 && hold_threshold == 0 && hold_blocks == 0; }
    friend bool operator==(const SafetyConfig&, const SafetyConfig&) = default;
};
// A loosening of the rules (higher limit, higher threshold, shorter hold, or switching a rule off)
// does not take effect at once: a thief with a stolen key must not be able to lift the limits instantly.
struct PendingSafety { bool active{false}; SafetyConfig config{}; std::uint64_t ready_height{0}; };
struct Account {
    Amount balance{0};
    std::uint64_t nonce{0};
    SafetyConfig safety{};
    std::uint64_t window_index{0};    // which limit window window_spent belongs to (only meaningful while limited)
    Amount window_spent{0};
    PendingSafety pending{};
    bool has_extension() const noexcept { return !safety.is_default() || window_spent != 0 || pending.active; }
    bool is_default() const noexcept { return balance == 0 && nonce == 0 && !has_extension(); }
};
struct HeldTransfer { Address from{}; Address to{}; Amount amount{0}; std::uint64_t release_height{0}; };
struct HeldEvent { Hash32 txid{}; Address from{}; Address to{}; Amount amount{0}; bool refunded{false}; };
struct ApplyResult { bool ok{false}; std::string error; Amount burned{0}; bool held{false}; };

class State {
public:
    // Canonical form: an account with nothing in it is never stored, so "absent" and "empty"
    // always produce the same state root on every node.
    void set_balance(const Address& address, Amount amount);
    Amount balance(const Address& address) const;
    std::uint64_t nonce(const Address& address) const;
    const Account* find(const Address& address) const;
    std::size_t account_count() const noexcept { return accounts_.size(); }
    Amount burned_total() const noexcept { return burned_; }
    Amount circulating() const;       // spendable balances only
    Amount held_total() const;        // funds waiting inside held transfers
    const std::map<Hash32, HeldTransfer>& held() const noexcept { return held_; }
    // Must be called once at the start of every block, before its transactions.
    std::vector<HeldEvent> begin_block(std::uint64_t height);
    ApplyResult apply(const Transaction& tx, std::uint64_t current_height);
    Hash32 state_root() const;
private:
    std::map<Address, Account> accounts_;   // std::map = the same canonical order on every node
    std::map<Hash32, HeldTransfer> held_;   // keyed by the txid of the held transfer
    Amount burned_{0};
};
}
