#include "taz/hello.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <vector>

namespace taz {
namespace {

bool is_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

int nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

bool parse_hex32(const std::string& s, Hash32& out) {
    if (s.size() != 64 || !is_hex(s)) return false;
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>((nib(s[2 * i]) << 4) | nib(s[2 * i + 1]));
    return true;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s.size() > 20) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (v > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;   // overflow, refused
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

}  // namespace

std::string encode_hello(const Hello& hello) {
    std::string out = "TAZHELLO/1\n";
    out += "proto=" + std::to_string(hello.protocol_version) + "\n";
    out += "chain=" + hello.chain_id + "\n";
    out += "genesis=" + to_hex(hello.genesis_commitment) + "\n";
    out += "height=" + std::to_string(hello.best_height) + "\n";
    out += "root=" + to_hex(hello.state_root) + "\n";
    return out;
}

bool decode_hello(std::string_view text, Hello& out, std::string* why) {
    auto fail = [&](std::string message) {
        if (why) *why = std::move(message);
        return false;
    };
    if (text.empty()) return fail("empty message");
    if (text.size() > kMaxHelloBytes) return fail("message is too large");

    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') { lines.push_back(current); current.clear(); continue; }
        if (c == '\r') continue;
        current.push_back(c);
        if (current.size() > kMaxHelloLine) return fail("a line is too long");
    }
    if (!current.empty()) lines.push_back(current);
    if (lines.empty() || lines[0] != "TAZHELLO/1") return fail("not a TAZVERA hello message");

    Hello parsed;
    bool have_proto = false, have_chain = false, have_genesis = false, have_height = false, have_root = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("a line is not key=value");
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "proto") {
            if (have_proto) return fail("duplicate proto");
            std::uint64_t v = 0;
            if (!parse_u64(value, v) || v > std::numeric_limits<std::uint32_t>::max()) return fail("bad proto");
            parsed.protocol_version = static_cast<std::uint32_t>(v);
            have_proto = true;
        } else if (key == "chain") {
            if (have_chain) return fail("duplicate chain");
            if (value.empty() || value.size() > 64) return fail("bad chain id");
            parsed.chain_id = value;
            have_chain = true;
        } else if (key == "genesis") {
            if (have_genesis) return fail("duplicate genesis");
            if (!parse_hex32(value, parsed.genesis_commitment)) return fail("genesis must be 64 hex characters");
            have_genesis = true;
        } else if (key == "height") {
            if (have_height) return fail("duplicate height");
            if (!parse_u64(value, parsed.best_height)) return fail("bad height");
            have_height = true;
        } else if (key == "root") {
            if (have_root) return fail("duplicate root");
            if (!parse_hex32(value, parsed.state_root)) return fail("root must be 64 hex characters");
            have_root = true;
        } else {
            return fail("unknown field \"" + key + "\"");
        }
    }
    if (!have_proto || !have_chain || !have_genesis || !have_height || !have_root)
        return fail("a required field is missing");
    out = parsed;
    return true;
}

HandshakeDecision check_peer(const Hello& ours, const Hello& theirs) {
    if (theirs.chain_id != ours.chain_id) return {false, "different chain ID"};
    if (theirs.protocol_version != ours.protocol_version) return {false, "different protocol version"};
    if (theirs.genesis_commitment != ours.genesis_commitment) return {false, "different genesis commitment"};
    return {true, {}};
}

Hello hello_for(const Chain& chain) {
    Hello h;
    h.protocol_version = PROTOCOL_VERSION;
    h.chain_id = CHAIN_ID;
    h.genesis_commitment = chain.genesis_commitment();
    h.best_height = chain.height();
    h.state_root = chain.state().state_root();
    return h;
}

}  // namespace taz
