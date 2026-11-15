#include "taz/persist.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace taz {
namespace {

struct Cursor {
    const std::string& s;
    std::size_t i{0};
    std::string err;
    bool fail(const char* m) { err = m; return false; }
    void skip() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool eof() { skip(); return i >= s.size(); }
    bool take(char c) {
        skip();
        if (i >= s.size() || s[i] != c) return false;
        ++i;
        return true;
    }
    bool expect(char c) {
        if (!take(c)) return fail(c == '{' ? "expected {" : "expected delimiter");
        return true;
    }
};

bool parse_string(Cursor& c, std::string& out) {
    c.skip();
    if (c.i >= c.s.size() || c.s[c.i] != '"') return c.fail("expected string");
    ++c.i;
    out.clear();
    while (c.i < c.s.size()) {
        char ch = c.s[c.i++];
        if (ch == '"') return true;
        if (ch == '\\') {
            if (c.i >= c.s.size()) return c.fail("bad escape");
            char e = c.s[c.i++];
            if (e == '"' || e == '\\' || e == '/') out.push_back(e);
            else if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else if (e == 'u') {
                if (c.i + 4 > c.s.size()) return c.fail("bad unicode escape");
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = c.s[c.i++];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                    else return c.fail("bad unicode escape");
                }
                if (v > 0x7f) return c.fail("only ASCII unicode escapes supported");
                out.push_back(static_cast<char>(v));
            } else return c.fail("bad escape");
        } else if (static_cast<unsigned char>(ch) < 0x20) return c.fail("raw control in string");
        else out.push_back(ch);
    }
    return c.fail("unterminated string");
}

bool parse_u64_string(Cursor& c, std::uint64_t& out) {
    std::string str;
    if (!parse_string(c, str)) return false;
    if (str.empty() || str.size() > 20) return c.fail("invalid u64 string");
    std::uint64_t v = 0;
    for (char ch : str) {
        if (ch < '0' || ch > '9') return c.fail("invalid u64 digits");
        std::uint64_t d = static_cast<std::uint64_t>(ch - '0');
        if (v > (UINT64_MAX - d) / 10) return c.fail("u64 overflow");
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool parse_i64_string(Cursor& c, std::int64_t& out) {
    std::string str;
    if (!parse_string(c, str)) return false;
    if (str.empty() || str == "-" || str.size() > 21) return c.fail("invalid i64 string");
    std::size_t p = 0;
    bool neg = false;
    if (str[0] == '-') { neg = true; p = 1; }
    std::uint64_t v = 0;
    for (; p < str.size(); ++p) {
        char ch = str[p];
        if (ch < '0' || ch > '9') return c.fail("invalid i64 digits");
        std::uint64_t d = static_cast<std::uint64_t>(ch - '0');
        if (v > (static_cast<std::uint64_t>(INT64_MAX) - d) / 10) return c.fail("i64 overflow");
        v = v * 10 + d;
    }
    if (neg) {
        if (v > static_cast<std::uint64_t>(INT64_MAX) + 1) return c.fail("i64 overflow");
        out = v == static_cast<std::uint64_t>(INT64_MAX) + 1 ? INT64_MIN : -static_cast<std::int64_t>(v);
    } else out = static_cast<std::int64_t>(v);
    return true;
}

int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool parse_hex_into(Cursor& c, std::uint8_t* dest, std::size_t n) {
    std::string str;
    if (!parse_string(c, str)) return false;
    if (str.size() != n * 2) return c.fail("hex length mismatch");
    for (std::size_t i = 0; i < n; ++i) {
        int hi = hex_val(str[i * 2]), lo = hex_val(str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return c.fail("invalid hex");
        dest[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

template <std::size_t N>
bool parse_hex_arr(Cursor& c, std::array<std::uint8_t, N>& a) {
    return parse_hex_into(c, a.data(), N);
}

bool key(Cursor& c, const char* name) {
    std::string k;
    if (!parse_string(c, k)) return false;
    if (k != name) return c.fail("unexpected key");
    if (!c.take(':')) return c.fail("expected :");
    return true;
}

bool parse_tx(Cursor& c, Transaction& tx) {
    if (!c.expect('{')) return false;
    Hash32 declared_txid{};
    if (!key(c, "txid") || !parse_hex_arr(c, declared_txid)) return false;
    if (!c.take(',')) return c.fail("tx comma");
    std::uint64_t u = 0;
    if (!key(c, "protocol_version") || !parse_u64_string(c, u) || u > UINT32_MAX) return false;
    tx.protocol_version = static_cast<std::uint32_t>(u);
    if (!c.take(',')) return false;
    if (!key(c, "chain_id") || !parse_string(c, tx.chain_id)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "kind") || !parse_u64_string(c, u) || u > 5) return c.fail("bad kind");
    tx.kind = static_cast<TxKind>(u);
    if (!c.take(',')) return false;
    if (!key(c, "from") || !parse_hex_arr(c, tx.from)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "to") || !parse_hex_arr(c, tx.to)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "amount") || !parse_u64_string(c, tx.amount)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "fee") || !parse_u64_string(c, tx.fee)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "nonce") || !parse_u64_string(c, tx.nonce)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "expiry_height") || !parse_u64_string(c, tx.expiry_height)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "daily_limit") || !parse_u64_string(c, tx.daily_limit)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "hold_threshold") || !parse_u64_string(c, tx.hold_threshold)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "hold_blocks") || !parse_u64_string(c, tx.hold_blocks)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "ref") || !parse_hex_arr(c, tx.ref)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "new_owner") || !parse_hex_arr(c, tx.new_owner)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "veto") || !parse_hex_arr(c, tx.veto)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "guardian_threshold") || !parse_u64_string(c, tx.guardian_threshold)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "guardians")) return false;
    if (!c.expect('[')) return false;
    tx.guardians.clear();
    c.skip();
    if (!c.take(']')) {
        for (;;) {
            Address g{};
            if (!parse_hex_arr(c, g)) return false;
            tx.guardians.push_back(g);
            c.skip();
            if (c.take(']')) break;
            if (!c.take(',')) return c.fail("guardians comma");
        }
    }
    if (!c.take(',')) return false;
    if (!key(c, "pubkey") || !parse_hex_arr(c, tx.pubkey)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "signature") || !parse_hex_arr(c, tx.signature)) return false;
    if (!c.expect('}')) return false;
    if (tx.txid() != declared_txid) return c.fail("txid mismatch");
    return true;
}

bool parse_block(Cursor& c, Block& b, Hash32& declared_hash) {
    if (!c.expect('{')) return false;
    std::uint64_t u = 0;
    if (!key(c, "height") || !parse_u64_string(c, b.height)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "protocol_version") || !parse_u64_string(c, u) || u > UINT32_MAX) return false;
    b.protocol_version = static_cast<std::uint32_t>(u);
    if (!c.take(',')) return false;
    if (!key(c, "chain_id") || !parse_string(c, b.chain_id)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "previous_hash") || !parse_hex_arr(c, b.previous_hash)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "timestamp") || !parse_i64_string(c, b.timestamp)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "state_root") || !parse_hex_arr(c, b.state_root)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "hash") || !parse_hex_arr(c, declared_hash)) return false;
    if (!c.take(',')) return false;
    if (!key(c, "transactions")) return false;
    if (!c.expect('[')) return false;
    b.transactions.clear();
    c.skip();
    if (!c.take(']')) {
        for (;;) {
            Transaction tx;
            if (!parse_tx(c, tx)) return false;
            b.transactions.push_back(std::move(tx));
            c.skip();
            if (c.take(']')) break;
            if (!c.take(',')) return c.fail("tx list comma");
        }
    }
    if (!c.expect('}')) return false;
    return true;
}

bool rebuild_chain(const std::vector<std::pair<Address, Amount>>& genesis,
                   const std::vector<std::pair<Block, Hash32>>& blocks,
                   Chain& out, std::string& err) {
    if (genesis.size() != 2) { err = "genesis_balances must list exactly 2 accounts"; return false; }
    if (blocks.empty()) { err = "missing genesis block"; return false; }
    Chain chain;
    if (!chain.init_genesis(genesis[0].first, genesis[0].second, genesis[1].first, genesis[1].second)) {
        err = "init_genesis failed";
        return false;
    }
    const Block& gfile = blocks[0].first;
    const Hash32& ghash = blocks[0].second;
    if (gfile.height != 0) { err = "first block must be height 0"; return false; }
    if (gfile.protocol_version != PROTOCOL_VERSION) { err = "unsupported protocol version"; return false; }
    if (gfile.chain_id != CHAIN_ID) { err = "wrong chain_id"; return false; }
    if (!gfile.transactions.empty()) { err = "genesis block must have no transactions"; return false; }
    if (gfile.timestamp != 0) { err = "genesis timestamp must be 0"; return false; }
    if (chain.blocks().front().state_root != gfile.state_root) { err = "genesis state_root mismatch"; return false; }
    if (chain.blocks().front().hash() != ghash) { err = "genesis hash mismatch"; return false; }
    if (chain.blocks().front().hash() != gfile.previous_hash && gfile.previous_hash != Hash32{}) {
        // genesis previous_hash is zero in normal export
    }
    if (gfile.previous_hash != Hash32{}) { err = "genesis previous_hash must be zero"; return false; }

    for (std::size_t i = 1; i < blocks.size(); ++i) {
        Block b = blocks[i].first;
        const Hash32& declared = blocks[i].second;
        if (b.hash() != declared) { err = "block hash mismatch at height " + std::to_string(b.height); return false; }
        auto r = chain.accept_block(b, NO_TIMESTAMP_LIMIT);
        if (!r.ok) { err = "accept_block failed at height " + std::to_string(b.height) + ": " + r.error; return false; }
        if (chain.blocks().back().hash() != declared) {
            err = "committed block hash mismatch";
            return false;
        }
    }
    out = std::move(chain);
    return true;
}

bool parse_document(const std::string& json, Chain& out, std::string& err) {
    Cursor c{json};
    if (!c.expect('{')) { err = c.err.empty() ? "expected object" : c.err; return false; }

    std::string format;
    if (!key(c, "format") || !parse_string(c, format)) { err = c.err; return false; }
    // v1 = transfers only, v2 = safety, v3 = guardians — same protocol_version, different feature use.
    if (format != "tazvera-chain-export-v1" && format != "tazvera-chain-export-v2" &&
        format != "tazvera-chain-export-v3") {
        err = "unsupported export format (need tazvera-chain-export-v1/v2/v3)";
        return false;
    }
    if (!c.take(',')) { err = "comma after format"; return false; }

    std::string chain_id;
    if (!key(c, "chain_id") || !parse_string(c, chain_id)) { err = c.err; return false; }
    if (chain_id != CHAIN_ID) { err = "chain_id mismatch"; return false; }
    if (!c.take(',')) { err = "comma"; return false; }

    std::uint64_t proto = 0;
    if (!key(c, "protocol_version") || !parse_u64_string(c, proto)) { err = c.err; return false; }
    if (proto != PROTOCOL_VERSION) { err = "protocol_version mismatch"; return false; }
    if (!c.take(',')) { err = "comma"; return false; }

    // params object — required keys checked lightly; values must match compiled constants
    if (!key(c, "params")) { err = c.err; return false; }
    if (!c.expect('{')) { err = c.err; return false; }
    auto need_u64 = [&](const char* name, std::uint64_t expected) -> bool {
        std::uint64_t v = 0;
        if (!key(c, name) || !parse_u64_string(c, v)) return false;
        if (v != expected) return c.fail("params constant mismatch");
        return true;
    };
    if (!need_u64("fee_transfer_v1", FEE_TRANSFER_V1) || !c.take(',')) { err = c.err.empty() ? "params" : c.err; return false; }
    if (!need_u64("max_block_txs", MAX_BLOCK_TXS) || !c.take(',')) { err = c.err.empty() ? "params" : c.err; return false; }
    if (!need_u64("limit_window_blocks", LIMIT_WINDOW_BLOCKS) || !c.take(',')) { err = c.err; return false; }
    if (!need_u64("max_hold_blocks", MAX_HOLD_BLOCKS) || !c.take(',')) { err = c.err; return false; }
    if (!need_u64("safety_delay_blocks", SAFETY_DELAY_BLOCKS) || !c.take(',')) { err = c.err; return false; }
    if (!need_u64("max_guardians", MAX_GUARDIANS) || !c.take(',')) { err = c.err; return false; }
    if (!need_u64("recovery_delay_blocks", RECOVERY_DELAY_BLOCKS)) { err = c.err; return false; }
    if (!c.expect('}')) { err = c.err; return false; }
    if (!c.take(',')) { err = "comma after params"; return false; }

    if (!key(c, "genesis_balances")) { err = c.err; return false; }
    if (!c.expect('[')) { err = c.err; return false; }
    std::vector<std::pair<Address, Amount>> genesis;
    c.skip();
    if (!c.take(']')) {
        for (;;) {
            if (!c.expect('{')) { err = c.err; return false; }
            Address a{}; Amount bal = 0;
            if (!key(c, "address") || !parse_hex_arr(c, a)) { err = c.err; return false; }
            if (!c.take(',')) { err = "comma"; return false; }
            if (!key(c, "balance") || !parse_u64_string(c, bal)) { err = c.err; return false; }
            if (!c.expect('}')) { err = c.err; return false; }
            genesis.emplace_back(a, bal);
            c.skip();
            if (c.take(']')) break;
            if (!c.take(',')) { err = "genesis comma"; return false; }
        }
    }
    if (!c.take(',')) { err = "comma after genesis"; return false; }

    if (!key(c, "blocks")) { err = c.err; return false; }
    if (!c.expect('[')) { err = c.err; return false; }
    std::vector<std::pair<Block, Hash32>> blocks;
    c.skip();
    if (!c.take(']')) {
        for (;;) {
            Block b; Hash32 h{};
            if (!parse_block(c, b, h)) { err = c.err; return false; }
            blocks.emplace_back(std::move(b), h);
            c.skip();
            if (c.take(']')) break;
            if (!c.take(',')) { err = "blocks comma"; return false; }
        }
    }
    if (!c.expect('}')) { err = c.err.empty() ? "expected end of object" : c.err; return false; }
    if (!c.eof()) { err = "trailing garbage after JSON"; return false; }

    return rebuild_chain(genesis, blocks, out, err);
}

}  // namespace

bool save_chain_json(const Chain& chain, const std::string& path, std::string* error) {
    try {
        const std::string body = to_json(chain);
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { if (error) *error = "could not open file for write"; return false; }
            out << body;
            out.flush();
            if (!out) { std::remove(tmp.c_str()); if (error) *error = "write failed"; return false; }
        }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            if (error) *error = "could not replace the chain file";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool load_chain_json_string(const std::string& json, Chain& out, std::string* error) {
    std::string err;
    Chain built;
    if (!parse_document(json, built, err)) {
        if (error) *error = err;
        return false;
    }
    out = std::move(built);
    return true;
}

bool load_chain_json(const std::string& path, Chain& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "could not open file for read"; return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof()) { if (error) *error = "read failed"; return false; }
    return load_chain_json_string(ss.str(), out, error);
}

}  // namespace taz
