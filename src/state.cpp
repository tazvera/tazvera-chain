#include "taz/state.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <stdexcept>
namespace taz {
namespace {
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) { for(int i=7;i>=0;--i) out.push_back(static_cast<std::uint8_t>((v>>(i*8))&0xffU)); }
}
bool State::set_genesis_balance(const Address& address, Amount amount) {
    if (accounts_.contains(address) || amount > MAX_SUPPLY) return false;
    Amount total=amount;
    for (const auto& [_, account] : accounts_) {
        if (account.balance > MAX_SUPPLY-total) return false;
        total += account.balance;
    }
    if (total > MAX_SUPPLY) return false;
    accounts_.emplace(address,Account{amount,0}); return true;
}
Amount State::balance(const Address& address) const { auto it=accounts_.find(address); return it==accounts_.end()?0:it->second.balance; }
std::uint64_t State::nonce(const Address& address) const { auto it=accounts_.find(address); return it==accounts_.end()?0:it->second.nonce; }
Amount State::circulating() const {
    Amount total=0;
    for(const auto& [_,account]:accounts_) { if(account.balance>std::numeric_limits<Amount>::max()-total) throw std::overflow_error("circulating supply overflow"); total+=account.balance; }
    return total;
}
bool State::supply_invariant() const {
    const Amount liquid=circulating();
    return burned_ <= MAX_SUPPLY && liquid <= MAX_SUPPLY-burned_;
}
ApplyResult State::apply(const Transaction& tx, std::uint64_t current_height) {
    ApplyResult result;
    if(auto error=validate_tx_shape(tx)){result.error=*error;return result;}
    if(tx.expiry_height!=0 && current_height>tx.expiry_height){result.error="transaction expired";return result;}
    const auto message=tx.signable_bytes();
    if(!verify(tx.pubkey,message,tx.signature)){result.error="invalid Ed25519 signature";return result;}
    const auto sender_it=accounts_.find(tx.from);
    const Account sender=sender_it==accounts_.end()?Account{}:sender_it->second;
    const auto receiver_it=accounts_.find(tx.to);
    const Account receiver=receiver_it==accounts_.end()?Account{}:receiver_it->second;
    if(tx.nonce!=sender.nonce){result.error="bad nonce";return result;}
    if(sender.nonce==std::numeric_limits<std::uint64_t>::max()){result.error="nonce overflow";return result;}
    const Amount required=tx.amount+tx.fee;
    if(sender.balance<required){result.error="insufficient balance";return result;}
    if(tx.amount>std::numeric_limits<Amount>::max()-receiver.balance){result.error="receiver balance overflow";return result;}
    if(tx.fee>std::numeric_limits<Amount>::max()-burned_){result.error="burn total overflow";return result;}
    const Amount new_burned=burned_+tx.fee;
    const Amount new_sender=sender.balance-required;
    const Amount new_receiver=receiver.balance+tx.amount;
    const Amount old_total=circulating()+burned_;
    const Amount new_total=(old_total-required)+tx.amount+tx.fee;
    if(new_total!=old_total || new_burned>MAX_SUPPLY){result.error="supply invariant violation";return result;}
    // All validation precedes mutation. The chain also applies blocks to a copy.
    accounts_[tx.from]=Account{new_sender,sender.nonce+1};
    accounts_[tx.to]=Account{new_receiver,receiver.nonce};
    burned_=new_burned;
    result.ok=true; result.burned=tx.fee; return result;
}
Hash32 State::state_root() const {
    std::vector<std::uint8_t> bytes; constexpr std::string_view domain="TAZVERA-STATE-V1";
    bytes.insert(bytes.end(),domain.begin(),domain.end());
    for(const auto& [address,account]:accounts_){bytes.insert(bytes.end(),address.begin(),address.end());put_u64(bytes,account.balance);put_u64(bytes,account.nonce);}
    put_u64(bytes,burned_); return sha256(bytes);
}
}
