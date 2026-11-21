#include "taz/hello.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <vector>

namespace taz {
namespace {

template <std::size_t N>
bool all_zero(const std::array<std::uint8_t, N>& a) {
    for (std::uint8_t b : a)
        if (b != 0) return false;
    return true;
}

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

template <std::size_t N>
bool parse_hex_n(const std::string& s, std::array<std::uint8_t, N>& out) {
    if (s.size() != N * 2 || !is_hex(s)) return false;
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<std::uint8_t>((nib(s[2 * i]) << 4) | nib(s[2 * i + 1]));
    return true;
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
    out += "tip=" + to_hex(hello.tip_hash) + "\n";
    out += "nonce=" + to_hex(hello.nonce) + "\n";
    if (hello_has_identity(hello)) {
        out += "pubkey=" + to_hex(hello.pubkey) + "\n";
        if (!all_zero(hello.signature)) out += "signature=" + to_hex(hello.signature) + "\n";
    }
    return out;
}

std::string hello_signable_bytes(const Hello& hello) {
    // The same text the peer will receive, minus the signature line: what is signed is exactly what is read.
    Hello copy = hello;
    copy.signature = Signature{};
    return encode_hello(copy);
}

bool sign_hello(Hello& hello, const PrivKey& private_seed) {
    if (!public_from_private(private_seed, hello.pubkey)) return false;
    const std::string bytes = hello_signable_bytes(hello);
    return sign(private_seed, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                           bytes.size()),
                hello.signature);
}

bool hello_has_identity(const Hello& hello) { return !all_zero(hello.pubkey); }

bool hello_signature_ok(const Hello& hello) {
    if (!hello_has_identity(hello) || all_zero(hello.signature)) return false;
    const std::string bytes = hello_signable_bytes(hello);
    return verify(hello.pubkey,
                  std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()),
                  hello.signature);
}

Address hello_address(const Hello& hello) { return address_from_pubkey(hello.pubkey); }

bool hello_has_nonce(const Hello& hello) { return !all_zero(hello.nonce); }

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
    bool have_proto = false, have_chain = false, have_genesis = false, have_height = false;
    bool have_root = false, have_tip = false, have_pubkey = false, have_signature = false, have_nonce = false;
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
        } else if (key == "tip") {
            if (have_tip) return fail("duplicate tip");
            if (!parse_hex32(value, parsed.tip_hash)) return fail("tip must be 64 hex characters");
            have_tip = true;
        } else if (key == "nonce") {
            if (have_nonce) return fail("duplicate nonce");
            if (!parse_hex32(value, parsed.nonce)) return fail("nonce must be 64 hex characters");
            have_nonce = true;
        } else if (key == "pubkey") {
            if (have_pubkey) return fail("duplicate pubkey");
            if (!parse_hex32(value, parsed.pubkey)) return fail("pubkey must be 64 hex characters");
            have_pubkey = true;
        } else if (key == "signature") {
            if (have_signature) return fail("duplicate signature");
            if (!parse_hex_n(value, parsed.signature)) return fail("signature must be 128 hex characters");
            have_signature = true;
        } else {
            return fail("unknown field \"" + key + "\"");
        }
    }
    if (!have_proto || !have_chain || !have_genesis || !have_height || !have_root || !have_tip || !have_nonce)
        return fail("a required field is missing");
    // Identity is optional, but half of it is a mistake: a key with no signature proves nothing, and a signature
    // with no key cannot be checked.
    if (have_pubkey != have_signature) return fail("pubkey and signature must be sent together");
    out = parsed;
    return true;
}

HandshakeDecision check_peer(const Hello& ours, const Hello& theirs) {
    if (theirs.chain_id != ours.chain_id) return {false, "different chain ID"};
    if (theirs.protocol_version != ours.protocol_version) return {false, "different protocol version"};
    if (theirs.genesis_commitment != ours.genesis_commitment) return {false, "different genesis commitment"};
    // v9.2: an all-zero nonce is what a failed random read leaves behind (or what a lazy peer sends). It would make
    // any identity proof on this connection replayable, so it is not accepted from the peer either.
    if (!hello_has_nonce(theirs)) return {false, "peer sent an all-zero nonce"};
    return {true, {}};
}

Hello hello_for(const Chain& chain) {
    Hello h;
    h.protocol_version = PROTOCOL_VERSION;
    h.chain_id = CHAIN_ID;
    h.genesis_commitment = chain.genesis_commitment();
    h.best_height = chain.height();
    // A failed random read leaves the nonce at zero, which hello_has_nonce() reports; the node refuses to use
    // such a hello on a connection rather than signing something predictable.
    if (!random_bytes(h.nonce)) return h;
    h.state_root = chain.state().state_root();
    h.tip_hash = chain.blocks().empty() ? Hash32{} : chain.blocks().back().hash();
    return h;
}

}  // namespace taz
