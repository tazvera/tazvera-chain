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
    std::string j;
    j += "{\n  \"format\": \"tazvera-chain-export-v1\",\n";
    j += "  \"chain_id\": " + q(CHAIN_ID) + ",\n";
    j += "  \"protocol_version\": " + num(PROTOCOL_VERSION) + ",\n";
    j += "  \"params\": {\"fee_transfer_v1\": " + num(FEE_TRANSFER_V1) + ", \"max_block_txs\": " + num(MAX_BLOCK_TXS) + "},\n";
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
            j += "\"from\": " + hex(tx.from) + ", \"to\": " + hex(tx.to) + ", ";
            j += "\"amount\": " + num(tx.amount) + ", \"fee\": " + num(tx.fee) + ", ";
            j += "\"nonce\": " + num(tx.nonce) + ", \"expiry_height\": " + num(tx.expiry_height) + ", ";
            j += "\"pubkey\": " + hex(tx.pubkey) + ", \"signature\": " + hex(tx.signature) + "}";
        }
        j += b.transactions.empty() ? "]\n    }" : "\n      ]\n    }";
    }
    j += "\n  ]\n}\n";
    return j;
}

WalletStatement wallet_statement(const Chain& chain, const Address& address) {
    WalletStatement s;
    for (const auto& [addr, bal] : chain.genesis_balances())
        if (addr == address) s.balance += bal;
    for (const Block& b : chain.blocks()) {
        for (const Transaction& tx : b.transactions) {
            if (tx.from == address) {
                s.balance -= tx.amount + tx.fee; s.nonce += 1;
                s.total_sent += tx.amount; s.total_fees += tx.fee;
                s.entries.push_back({b.height, tx.txid(), tx.to, false, tx.amount, tx.fee});
            }
            if (tx.to == address) {
                s.balance += tx.amount; s.total_received += tx.amount;
                s.entries.push_back({b.height, tx.txid(), tx.from, true, tx.amount, 0});
            }
        }
    }
    return s;
}
}
