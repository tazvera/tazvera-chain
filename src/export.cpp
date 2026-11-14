#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <algorithm>

namespace taz {
namespace {
std::string json_escape(std::string_view s) {
    static constexpr char hexd[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { out += "\\u00"; out += hexd[c >> 4]; out += hexd[c & 15]; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}
std::string q(std::string_view s) { return "\"" + json_escape(s) + "\""; }
template <class T> std::string hex(const T& bytes) { return q(to_hex(std::span<const std::uint8_t>(bytes))); }
std::string num(std::uint64_t v) { return q(std::to_string(v)); }
std::string snum(std::int64_t v) { return q(std::to_string(v)); }
}

std::string to_json(const Chain& chain) {
    bool uses_safety = false;
    for (const Block& b : chain.blocks()) for (const Transaction& tx : b.transactions) if (tx.kind != TxKind::Transfer) uses_safety = true;
    std::string j;
    j += std::string("{\n  \"format\": \"") + (uses_safety ? "tazvera-chain-export-v2" : "tazvera-chain-export-v1") + "\",\n";
    j += "  \"chain_id\": " + q(CHAIN_ID) + ",\n";
    j += "  \"protocol_version\": " + num(PROTOCOL_VERSION) + ",\n";
    j += "  \"params\": {\"fee_transfer_v1\": " + num(FEE_TRANSFER_V1) + ", \"max_block_txs\": " + num(MAX_BLOCK_TXS)
       + ", \"limit_window_blocks\": " + num(LIMIT_WINDOW_BLOCKS) + ", \"max_hold_blocks\": " + num(MAX_HOLD_BLOCKS)
       + ", \"safety_delay_blocks\": " + num(SAFETY_DELAY_BLOCKS) + "},\n";
    j += "  \"genesis_balances\": [";
    bool first = true;
    for (const auto& [addr, bal] : chain.genesis_balances()) {
        j += std::string(first ? "" : ",") + "\n    {\"address\": " + hex(addr) + ", \"balance\": " + num(bal) + "}";
        first = false;
    }
    j += "\n  ],\n  \"blocks\": [";
    for (std::size_t i = 0; i < chain.blocks().size(); ++i) {
        const Block& b = chain.blocks()[i];
        j += std::string(i ? "," : "") + "\n    {\n";
        j += "      \"height\": " + num(b.height) + ",\n";
        j += "      \"protocol_version\": " + num(b.protocol_version) + ",\n";
        j += "      \"chain_id\": " + q(b.chain_id) + ",\n";
        j += "      \"previous_hash\": " + hex(b.previous_hash) + ",\n";
        j += "      \"timestamp\": " + snum(b.timestamp) + ",\n";
        j += "      \"state_root\": " + hex(b.state_root) + ",\n";
        j += "      \"hash\": " + hex(b.hash()) + ",\n";
        j += "      \"transactions\": [";
        for (std::size_t t = 0; t < b.transactions.size(); ++t) {
            const Transaction& tx = b.transactions[t];
            j += std::string(t ? "," : "") + "\n        {";
            j += "\"txid\": " + hex(tx.txid()) + ", ";
            j += "\"protocol_version\": " + num(tx.protocol_version) + ", ";
            j += "\"chain_id\": " + q(tx.chain_id) + ", ";
            j += "\"kind\": " + num(static_cast<std::uint8_t>(tx.kind)) + ", ";
            j += "\"from\": " + hex(tx.from) + ", \"to\": " + hex(tx.to) + ", ";
            j += "\"amount\": " + num(tx.amount) + ", \"fee\": " + num(tx.fee) + ", ";
            j += "\"nonce\": " + num(tx.nonce) + ", \"expiry_height\": " + num(tx.expiry_height) + ", ";
            j += "\"daily_limit\": " + num(tx.daily_limit) + ", \"hold_threshold\": " + num(tx.hold_threshold) + ", ";
            j += "\"hold_blocks\": " + num(tx.hold_blocks) + ", \"ref\": " + hex(tx.ref) + ", ";
            j += "\"pubkey\": " + hex(tx.pubkey) + ", \"signature\": " + hex(tx.signature) + "}";
        }
        j += b.transactions.empty() ? "]\n    }" : "\n      ]\n    }";
    }
    j += "\n  ]\n}\n";
    return j;
}

WalletStatement wallet_statement(const Chain& chain, const Address& address) {
    WalletStatement s;
    State st;
    for (const auto& [addr, bal] : chain.genesis_balances()) st.set_balance(addr, bal);
    auto add = [&s](std::uint64_t h, const Hash32& id, const Address& other, EntryKind k, Amount amount, Amount fee) {
        s.entries.push_back(StatementEntry{h, id, other, k, amount, fee});
    };
    for (const Block& b : chain.blocks()) {
        if (b.height == 0) continue;
        for (const HeldEvent& ev : st.begin_block(b.height)) {
            if (ev.refunded) { if (ev.from == address) add(b.height, ev.txid, ev.to, EntryKind::Refunded, ev.amount, 0); }
            else if (ev.to == address) add(b.height, ev.txid, ev.from, EntryKind::ReleasedIn, ev.amount, 0);
        }
        for (const Transaction& tx : b.transactions) {
            std::optional<HeldTransfer> target;
            if (tx.kind == TxKind::CancelHeld) { auto it = st.held().find(tx.ref); if (it != st.held().end()) target = it->second; }
            const ApplyResult res = st.apply(tx, b.height);
            if (!res.ok) continue; // cannot happen for a chain that Chain accepted
            const Hash32 id = tx.txid();
            if (tx.from == address) {
                s.total_fees += tx.fee;
                if (tx.kind == TxKind::Transfer) add(b.height, id, tx.to, res.held ? EntryKind::HeldOut : EntryKind::Sent, tx.amount, tx.fee);
                else if (tx.kind == TxKind::SetSafety) add(b.height, id, Address{}, EntryKind::SafetyChanged, 0, tx.fee);
                else if (target) add(b.height, id, target->to, EntryKind::Cancelled, target->amount, tx.fee);
            }
            if (tx.kind == TxKind::Transfer && tx.to == address && !res.held) add(b.height, id, tx.from, EntryKind::Received, tx.amount, 0);
        }
    }
    s.balance = st.balance(address); s.nonce = st.nonce(address);
    for (const StatementEntry& e : s.entries) {
        switch (e.kind) {
            case EntryKind::Sent: case EntryKind::HeldOut: s.total_sent += e.amount; break;
            case EntryKind::Received: case EntryKind::ReleasedIn: s.total_received += e.amount; break;
            case EntryKind::Cancelled: case EntryKind::Refunded: s.total_refunded += e.amount; break;
            case EntryKind::SafetyChanged: break;
        }
    }
    for (const auto& [_, h] : st.held()) {
        if (h.from == address) s.held_outgoing += h.amount;
        if (h.to == address) s.held_incoming += h.amount;
    }
    return s;
}
}
