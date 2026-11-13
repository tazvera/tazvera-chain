#include "taz/state.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <stdexcept>
namespace taz {
namespace {
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffU));
}
}
void State::set_balance(const Address& address, Amount amount) {
    auto it = accounts_.find(address);
    if (amount == 0) {
        if (it != accounts_.end()) {
            it->second.balance = 0;
            if (it->second.nonce == 0) accounts_.erase(it); // keep the stored form canonical
        }
        return;
    }
    accounts_[address].balance = amount;
}
Amount State::balance(const Address& address) const {
    auto it = accounts_.find(address); return it == accounts_.end() ? 0 : it->second.balance;
}
std::uint64_t State::nonce(const Address& address) const {
    auto it = accounts_.find(address); return it == accounts_.end() ? 0 : it->second.nonce;
}
Amount State::circulating() const {
    Amount total = 0;
    for (const auto& [_, account] : accounts_) {
        if (account.balance > std::numeric_limits<Amount>::max() - total)
            throw std::overflow_error("circulating supply overflow");
        total += account.balance;
    }
    return total;
}
ApplyResult State::apply(const Transaction& tx, std::uint64_t current_height) {
    ApplyResult result;
    if (auto error = validate_tx_shape(tx)) { result.error = *error; return result; }
    if (tx.expiry_height != 0 && current_height > tx.expiry_height) { result.error = "transaction expired"; return result; }
    const auto message = tx.signable_bytes();
    if (!verify(tx.pubkey, message, tx.signature)) { result.error = "invalid Ed25519 signature"; return result; }
    const auto sender_it = accounts_.find(tx.from);
    const Account sender = sender_it == accounts_.end() ? Account{} : sender_it->second;
    const auto receiver_it = accounts_.find(tx.to);
    const Account receiver = receiver_it == accounts_.end() ? Account{} : receiver_it->second;
    if (tx.nonce != sender.nonce) { result.error = "bad nonce"; return result; }
    if (sender.nonce == std::numeric_limits<std::uint64_t>::max()) { result.error = "nonce overflow"; return result; }
    const Amount required = tx.amount + tx.fee; // shape check already proved this addition safe
    if (sender.balance < required) { result.error = "insufficient balance"; return result; }
    if (tx.amount > std::numeric_limits<Amount>::max() - receiver.balance) { result.error = "receiver balance overflow"; return result; }
    if (tx.fee > std::numeric_limits<Amount>::max() - burned_) { result.error = "burn total overflow"; return result; }
    // All checks complete before mutation: a rejected transfer changes nothing.
    // The sender always ends with nonce >= 1 and the receiver with amount > 0 (shape check),
    // so neither stored account can be empty.
    accounts_[tx.from].balance = sender.balance - required;
    accounts_[tx.from].nonce = sender.nonce + 1;
    accounts_[tx.to].balance = receiver.balance + tx.amount;
    burned_ += tx.fee;
    result.ok = true; result.burned = tx.fee; return result;
}
Hash32 State::state_root() const {
    std::vector<std::uint8_t> bytes;
    constexpr std::string_view domain = "TAZVERA-STATE-V1";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    for (const auto& [address, account] : accounts_) {
        bytes.insert(bytes.end(), address.begin(), address.end());
        put_u64(bytes, account.balance); put_u64(bytes, account.nonce);
    }
    put_u64(bytes, burned_);
    return sha256(bytes);
}
}
