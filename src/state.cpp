#include "taz/state.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace taz {
namespace {
constexpr Amount MAX_AMOUNT = std::numeric_limits<Amount>::max();
void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffU));
}
// True when the new rules protect LESS than the old ones. First-time setup (old = nothing) is not a loosening.
bool is_loosening(const SafetyConfig& o, const SafetyConfig& n) {
    if (o.is_default()) return false;
    const bool limit_looser = o.daily_limit != 0 && (n.daily_limit == 0 || n.daily_limit > o.daily_limit);
    const bool threshold_looser = o.hold_threshold != 0 && (n.hold_threshold == 0 || n.hold_threshold > o.hold_threshold);
    const bool hold_looser = o.hold_threshold != 0 && n.hold_threshold != 0 && n.hold_blocks < o.hold_blocks;
    return limit_looser || threshold_looser || hold_looser;
}
}
void State::set_balance(const Address& address, Amount amount) {
    auto it = accounts_.find(address);
    if (it == accounts_.end()) { if (amount != 0) accounts_[address].balance = amount; return; }
    it->second.balance = amount;
    if (it->second.is_default()) accounts_.erase(it); // keep the stored form canonical
}
Amount State::balance(const Address& address) const {
    auto it = accounts_.find(address); return it == accounts_.end() ? 0 : it->second.balance;
}
std::uint64_t State::nonce(const Address& address) const {
    auto it = accounts_.find(address); return it == accounts_.end() ? 0 : it->second.nonce;
}
const Account* State::find(const Address& address) const {
    auto it = accounts_.find(address); return it == accounts_.end() ? nullptr : &it->second;
}
Amount State::circulating() const {
    Amount total = 0;
    for (const auto& [_, account] : accounts_) {
        if (account.balance > MAX_AMOUNT - total) throw std::overflow_error("circulating supply overflow");
        total += account.balance;
    }
    return total;
}
Amount State::held_total() const {
    Amount total = 0;
    for (const auto& [_, h] : held_) {
        if (h.amount > MAX_AMOUNT - total) throw std::overflow_error("held total overflow");
        total += h.amount;
    }
    return total;
}
std::vector<HeldEvent> State::begin_block(std::uint64_t height) {
    std::vector<HeldEvent> events;
    // 1) release every hold that is due, in a fixed order: (release height, txid)
    std::vector<std::pair<std::uint64_t, Hash32>> due;
    for (const auto& [id, h] : held_) if (h.release_height <= height) due.emplace_back(h.release_height, id);
    std::sort(due.begin(), due.end());
    for (const auto& [_, id] : due) {
        const HeldTransfer h = held_.at(id);
        held_.erase(id);
        HeldEvent ev{id, h.from, h.to, h.amount, false};
        auto rit = accounts_.find(h.to);
        const Amount have = rit == accounts_.end() ? 0 : rit->second.balance;
        if (h.amount > MAX_AMOUNT - have) { // cannot credit the receiver: give the funds back to the sender
            accounts_[h.from].balance += h.amount; ev.refunded = true;
        } else {
            accounts_[h.to].balance = have + h.amount;
        }
        events.push_back(ev);
    }
    // 2) apply every loosening whose waiting time is over
    for (auto& [_, acc] : accounts_) {
        if (acc.pending.active && acc.pending.ready_height <= height) {
            acc.safety = acc.pending.config; acc.pending = PendingSafety{};
            if (acc.safety.daily_limit == 0) { acc.window_index = 0; acc.window_spent = 0; }
        }
    }
    return events;
}
ApplyResult State::apply(const Transaction& tx, std::uint64_t height) {
    ApplyResult result;
    auto fail = [&result](std::string msg) { result.error = std::move(msg); return result; };
    if (auto error = validate_tx_shape(tx)) return fail(*error);
    if (tx.expiry_height != 0 && height > tx.expiry_height) return fail("transaction expired");
    if (!verify(tx.pubkey, tx.signable_bytes(), tx.signature)) return fail("invalid Ed25519 signature");

    const auto sender_it = accounts_.find(tx.from);
    Account sender = sender_it == accounts_.end() ? Account{} : sender_it->second;
    if (tx.nonce != sender.nonce) return fail("bad nonce");
    if (sender.nonce == std::numeric_limits<std::uint64_t>::max()) return fail("nonce overflow");
    if (tx.fee > MAX_AMOUNT - burned_) return fail("burn total overflow");
    if (sender.balance < tx.fee) return fail("insufficient balance");

    // All checks for each kind come first; only then is anything written, so a rejected tx changes nothing.
    switch (tx.kind) {
    case TxKind::Transfer: {
        const Amount required = tx.amount + tx.fee; // shape check proved this addition safe
        if (sender.balance < required) return fail("insufficient balance");
        const auto rit = accounts_.find(tx.to);
        Account receiver = rit == accounts_.end() ? Account{} : rit->second;
        const std::uint64_t window = height / LIMIT_WINDOW_BLOCKS;
        const bool limited = sender.safety.daily_limit != 0;
        const Amount spent = (limited && sender.window_index == window) ? sender.window_spent : 0;
        if (limited && (spent > sender.safety.daily_limit || tx.amount > sender.safety.daily_limit - spent))
            return fail("daily limit exceeded");
        const bool hold = sender.safety.hold_threshold != 0 && tx.amount >= sender.safety.hold_threshold;
        Hash32 id{};
        if (hold) { id = tx.txid(); if (held_.contains(id)) return fail("duplicate held transfer"); }
        else if (tx.amount > MAX_AMOUNT - receiver.balance) return fail("receiver balance overflow");
        sender.balance -= required; sender.nonce += 1;
        if (limited) { sender.window_index = window; sender.window_spent = spent + tx.amount; }
        if (hold) held_[id] = HeldTransfer{tx.from, tx.to, tx.amount, height + sender.safety.hold_blocks};
        else { receiver.balance += tx.amount; accounts_[tx.to] = receiver; }
        accounts_[tx.from] = sender;
        result.held = hold;
        break;
    }
    case TxKind::SetSafety: {
        const SafetyConfig wanted{tx.daily_limit, tx.hold_threshold, tx.hold_blocks};
        sender.balance -= tx.fee; sender.nonce += 1;
        if (is_loosening(sender.safety, wanted)) {
            sender.pending = PendingSafety{true, wanted, height + SAFETY_DELAY_BLOCKS};
        } else { // tightening, unchanged or first-time: immediate. It also cancels any waiting loosening.
            sender.safety = wanted; sender.pending = PendingSafety{};
            if (wanted.daily_limit == 0) { sender.window_index = 0; sender.window_spent = 0; }
        }
        accounts_[tx.from] = sender;
        break;
    }
    case TxKind::CancelHeld: {
        const auto hit = held_.find(tx.ref);
        if (hit == held_.end() || hit->second.from != tx.from) return fail("no such held transfer for this account");
        const Amount after_fee = sender.balance - tx.fee;
        if (after_fee > MAX_AMOUNT - hit->second.amount) return fail("balance overflow on refund");
        sender.balance = after_fee + hit->second.amount; sender.nonce += 1;
        held_.erase(hit);
        accounts_[tx.from] = sender;
        break;
    }
    }
    burned_ += tx.fee;
    result.ok = true; result.burned = tx.fee; return result;
}
Hash32 State::state_root() const {
    bool extended = !held_.empty();
    for (const auto& [_, a] : accounts_) if (a.has_extension()) { extended = true; break; }
    std::vector<std::uint8_t> bytes;
    if (!extended) {
        // Identical to the v1 encoding, so chains that only use plain transfers keep their old roots.
        constexpr std::string_view domain = "TAZVERA-STATE-V1";
        bytes.insert(bytes.end(), domain.begin(), domain.end());
        for (const auto& [address, account] : accounts_) {
            bytes.insert(bytes.end(), address.begin(), address.end());
            put_u64(bytes, account.balance); put_u64(bytes, account.nonce);
        }
        put_u64(bytes, burned_);
        return sha256(bytes);
    }
    constexpr std::string_view domain = "TAZVERA-STATE-V2"; // fully length-prefixed: no two states share an encoding
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    put_u64(bytes, accounts_.size());
    for (const auto& [address, a] : accounts_) {
        bytes.insert(bytes.end(), address.begin(), address.end());
        put_u64(bytes, a.balance); put_u64(bytes, a.nonce);
        put_u64(bytes, a.safety.daily_limit); put_u64(bytes, a.safety.hold_threshold); put_u64(bytes, a.safety.hold_blocks);
        put_u64(bytes, a.window_index); put_u64(bytes, a.window_spent);
        put_u8(bytes, a.pending.active ? 1 : 0);
        put_u64(bytes, a.pending.config.daily_limit); put_u64(bytes, a.pending.config.hold_threshold);
        put_u64(bytes, a.pending.config.hold_blocks); put_u64(bytes, a.pending.ready_height);
    }
    put_u64(bytes, burned_);
    put_u64(bytes, held_.size());
    for (const auto& [id, h] : held_) {
        bytes.insert(bytes.end(), id.begin(), id.end());
        bytes.insert(bytes.end(), h.from.begin(), h.from.end()); bytes.insert(bytes.end(), h.to.begin(), h.to.end());
        put_u64(bytes, h.amount); put_u64(bytes, h.release_height);
    }
    return sha256(bytes);
}
}
