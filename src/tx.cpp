#include "taz/tx.hpp"
#include "taz/crypto.hpp"
#include <limits>
namespace taz {
namespace {
void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffU));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffU));
}
void put_string(std::vector<std::uint8_t>& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size())); out.insert(out.end(), s.begin(), s.end());
}
}
std::vector<std::uint8_t> Transaction::signable_bytes() const {
    std::vector<std::uint8_t> out;
    if (kind == TxKind::Transfer) {
        // Unchanged from v1 so that plain-transfer chains keep verifying everywhere.
        constexpr std::string_view domain = "TAZVERA-TX-SIGNATURE-V1";
        out.insert(out.end(), domain.begin(), domain.end());
        put_u32(out, protocol_version); put_string(out, chain_id);
        out.insert(out.end(), from.begin(), from.end()); out.insert(out.end(), to.begin(), to.end());
        put_u64(out, amount); put_u64(out, fee); put_u64(out, nonce); put_u64(out, expiry_height);
        out.insert(out.end(), pubkey.begin(), pubkey.end());
        return out;
    }
    // Other kinds use a different domain string, so a signature can never be replayed across kinds.
    constexpr std::string_view domain = "TAZVERA-TX-SIGNATURE-V2";
    out.insert(out.end(), domain.begin(), domain.end());
    put_u32(out, protocol_version); put_string(out, chain_id); put_u8(out, static_cast<std::uint8_t>(kind));
    out.insert(out.end(), from.begin(), from.end());
    put_u64(out, fee); put_u64(out, nonce); put_u64(out, expiry_height);
    out.insert(out.end(), pubkey.begin(), pubkey.end());
    if (kind == TxKind::SetSafety) { put_u64(out, daily_limit); put_u64(out, hold_threshold); put_u64(out, hold_blocks); }
    else out.insert(out.end(), ref.begin(), ref.end()); // CancelHeld
    return out;
}
Hash32 Transaction::txid() const {
    auto bytes = signable_bytes(); bytes.insert(bytes.end(), signature.begin(), signature.end());
    return sha256(bytes);
}
std::optional<std::string> validate_tx_shape(const Transaction& tx) {
    if (tx.protocol_version != PROTOCOL_VERSION) return "unsupported protocol version";
    if (tx.chain_id != CHAIN_ID) return "wrong chain ID";
    if (tx.fee != FEE_TRANSFER_V1) return "fee does not match fee schedule v1";
    if (address_from_pubkey(tx.pubkey) != tx.from) return "public key does not match sender address";
    const bool no_to = tx.to == Address{};
    const bool no_safety_fields = tx.daily_limit == 0 && tx.hold_threshold == 0 && tx.hold_blocks == 0;
    const bool no_ref = tx.ref == Hash32{};
    switch (tx.kind) {
        case TxKind::Transfer:
            if (tx.amount == 0) return "amount must be greater than zero";
            if (tx.from == tx.to) return "self-transfer is not supported in this prototype";
            if (tx.amount > std::numeric_limits<Amount>::max() - tx.fee) return "amount plus fee overflow";
            if (!no_safety_fields || !no_ref) return "unused fields must be zero in a transfer";
            return std::nullopt;
        case TxKind::SetSafety:
            if (tx.amount != 0 || !no_to || !no_ref) return "unused fields must be zero in a safety setting";
            if (tx.hold_threshold == 0 && tx.hold_blocks != 0) return "hold_blocks must be zero when nothing is held";
            if (tx.hold_threshold != 0 && (tx.hold_blocks == 0 || tx.hold_blocks > MAX_HOLD_BLOCKS)) return "hold_blocks out of range";
            return std::nullopt;
        case TxKind::CancelHeld:
            if (tx.amount != 0 || !no_to || !no_safety_fields) return "unused fields must be zero in a cancel";
            return std::nullopt;
    }
    return "unknown transaction kind";
}
}
