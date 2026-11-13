#include "taz/tx.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <stdexcept>
namespace taz {
namespace {
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int i=3;i>=0;--i) out.push_back(static_cast<std::uint8_t>((v>>(i*8))&0xffU)); }
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) { for (int i=7;i>=0;--i) out.push_back(static_cast<std::uint8_t>((v>>(i*8))&0xffU)); }
void put_string(std::vector<std::uint8_t>& out, const std::string& s) {
    if (s.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("chain ID too long");
    put_u32(out, static_cast<std::uint32_t>(s.size())); out.insert(out.end(), s.begin(), s.end());
}
}
std::vector<std::uint8_t> Transaction::signable_bytes() const {
    std::vector<std::uint8_t> out; constexpr std::string_view domain="TAZVERA-TX-SIGNATURE-V1";
    out.insert(out.end(),domain.begin(),domain.end()); put_u32(out,protocol_version); put_string(out,chain_id);
    out.insert(out.end(),from.begin(),from.end()); out.insert(out.end(),to.begin(),to.end());
    put_u64(out,amount); put_u64(out,fee); put_u64(out,nonce); put_u64(out,expiry_height);
    out.insert(out.end(),pubkey.begin(),pubkey.end()); return out;
}
Hash32 Transaction::txid() const {
    auto bytes=signable_bytes(); bytes.insert(bytes.end(),signature.begin(),signature.end()); return sha256(bytes);
}
std::optional<std::string> validate_tx_shape(const Transaction& tx) {
    if (tx.protocol_version != PROTOCOL_VERSION) return "unsupported protocol version";
    if (tx.chain_id != CHAIN_ID) return "wrong chain ID";
    if (tx.amount == 0) return "amount must be greater than zero";
    if (tx.fee != FEE_TRANSFER_V1) return "fee does not match prototype fee schedule";
    if (tx.from == tx.to) return "self-transfer is not supported";
    if (address_from_pubkey(tx.pubkey) != tx.from) return "public key does not match sender address";
    if (tx.amount > std::numeric_limits<Amount>::max() - tx.fee) return "amount plus fee overflow";
    return std::nullopt;
}
}
