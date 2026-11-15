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
    switch (kind) {
        case TxKind::SetSafety: put_u64(out, daily_limit); put_u64(out, hold_threshold); put_u64(out, hold_blocks); break;
        case TxKind::CancelHeld: out.insert(out.end(), ref.begin(), ref.end()); break;
        case TxKind::SetGuardians:
            put_u64(out, guardians.size());
            for (const Address& g : guardians) out.insert(out.end(), g.begin(), g.end());
            put_u64(out, guardian_threshold);
            break;
        case TxKind::RecoveryVote:
            out.insert(out.end(), to.begin(), to.end()); out.insert(out.end(), new_owner.begin(), new_owner.end()); break;
        case TxKind::RecoveryCancel: case TxKind::Transfer: break; // nothing more to sign
    }
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
    const bool no_new_owner = tx.new_owner == Address{};
    const bool no_guardians = tx.guardians.empty() && tx.guardian_threshold == 0;
    // Fields that a kind does not use must be zero, so no two different byte strings mean the same thing.
    const bool only_safety = tx.amount == 0 && no_to && no_ref && no_new_owner && no_guardians;
    switch (tx.kind) {
        case TxKind::Transfer:
            if (tx.amount == 0) return "amount must be greater than zero";
            if (tx.from == tx.to) return "self-transfer is not supported in this prototype";
            if (tx.amount > std::numeric_limits<Amount>::max() - tx.fee) return "amount plus fee overflow";
            if (!no_safety_fields || !no_ref || !no_new_owner || !no_guardians) return "unused fields must be zero in a transfer";
            return std::nullopt;
        case TxKind::SetSafety:
            if (!only_safety) return "unused fields must be zero in a safety setting";
            if (tx.hold_threshold == 0 && tx.hold_blocks != 0) return "hold_blocks must be zero when nothing is held";
            if (tx.hold_threshold != 0 && (tx.hold_blocks == 0 || tx.hold_blocks > MAX_HOLD_BLOCKS)) return "hold_blocks out of range";
            return std::nullopt;
        case TxKind::CancelHeld:
            if (tx.amount != 0 || !no_to || !no_safety_fields || !no_new_owner || !no_guardians) return "unused fields must be zero in a cancel";
            return std::nullopt;
        case TxKind::SetGuardians:
            if (tx.amount != 0 || !no_to || !no_ref || !no_safety_fields || !no_new_owner) return "unused fields must be zero in a guardian setting";
            if (tx.guardians.size() > MAX_GUARDIANS) return "too many guardians";
            if (tx.guardians.empty()) return tx.guardian_threshold == 0 ? std::nullopt : std::optional<std::string>("threshold must be zero without guardians");
            if (tx.guardian_threshold == 0 || tx.guardian_threshold > tx.guardians.size()) return "threshold must be between 1 and the number of guardians";
            for (std::size_t i = 0; i < tx.guardians.size(); ++i) {
                if (tx.guardians[i] == Address{}) return "guardian address is empty";
                if (tx.guardians[i] == tx.from) return "an account cannot be its own guardian";
                if (i > 0 && !(tx.guardians[i - 1] < tx.guardians[i])) return "guardians must be sorted and unique";
            }
            return std::nullopt;
        case TxKind::RecoveryVote:
            if (tx.amount != 0 || !no_safety_fields || !no_ref || !no_guardians) return "unused fields must be zero in a recovery vote";
            if (no_to) return "a recovery vote must name the account to recover";
            if (tx.to == tx.from) return "an account cannot vote on its own recovery";
            if (no_new_owner) return "a recovery vote must name the new address";
            if (tx.new_owner == tx.to) return "the new address must differ from the old one";
            return std::nullopt;
        case TxKind::RecoveryCancel:
            if (!only_safety || !no_safety_fields) return "unused fields must be zero in a recovery cancel";
            return std::nullopt;
    }
    return "unknown transaction kind";
}
}
