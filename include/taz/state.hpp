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
// The trusted people ("guardians") who may help the owner back into the account after the key is lost.
struct GuardianConfig {
    std::vector<Address> list{};      // strictly ascending
    std::uint64_t threshold{0};       // how many of them must agree
    Address veto{};                   // optional separate key that alone may cancel a recovery (zero = the account's own key may)
    bool is_default() const noexcept { return list.empty() && threshold == 0 && veto == Address{}; }
    friend bool operator==(const GuardianConfig&, const GuardianConfig&) = default;
};
// Changing the guardians is always delayed (a thief must not be able to name his own helpers at once).
struct PendingGuardians { bool active{false}; GuardianConfig config{}; std::uint64_t ready_height{0}; };
// An agreed recovery that is waiting out its delay; the owner may cancel until ready_height.
struct PendingRecovery {
    bool active{false};
    Address target{};
    std::uint64_t ready_height{0};
    bool veto_postponed{false}; // R2: veto may postpone once, not cancel forever
};
struct Account {
    Amount balance{0};
    std::uint64_t nonce{0};
    SafetyConfig safety{};
    std::uint64_t window_index{0};    // which limit window window_spent belongs to (only meaningful while limited)
    Amount window_spent{0};
    PendingSafety pending{};
    GuardianConfig guardians{};
    PendingGuardians pending_guardians{};
    std::map<Address, Address> votes{};   // guardian -> the new address he voted for (cleared when agreed)
    PendingRecovery recovery{};
    bool has_safety_extension() const noexcept { return !safety.is_default() || window_spent != 0 || pending.active; }
    bool has_guardian_extension() const noexcept {
        return !guardians.is_default() || pending_guardians.active || !votes.empty() || recovery.active;
    }
    bool has_extension() const noexcept { return has_safety_extension() || has_guardian_extension(); }
    bool is_default() const noexcept { return balance == 0 && nonce == 0 && !has_extension(); }
};
struct HeldTransfer { Address from{}; Address to{}; Amount amount{0}; std::uint64_t release_height{0}; };
struct HeldEvent { Hash32 txid{}; Address from{}; Address to{}; Amount amount{0}; bool refunded{false}; };
// A recovery that came due: the whole spendable balance moved (or, if the new address could not hold it, nothing did).
struct RecoveryEvent { Address from{}; Address to{}; Amount amount{0}; bool moved{false}; };
struct BlockEvents { std::vector<HeldEvent> held; std::vector<RecoveryEvent> recoveries; };
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
    BlockEvents begin_block(std::uint64_t height);
    ApplyResult apply(const Transaction& tx, std::uint64_t current_height);
    Hash32 state_root() const;
private:
    std::map<Address, Account> accounts_;   // std::map = the same canonical order on every node
    std::map<Hash32, HeldTransfer> held_;   // keyed by the txid of the held transfer
    Amount burned_{0};
};
}
