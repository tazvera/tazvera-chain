#include "taz/auth.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace taz {
namespace {

void put_u32be(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void put_bytes(std::string& out, std::span<const std::uint8_t> bytes) {
    out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A field that is too long is a protocol error, not something to truncate.
constexpr std::size_t kMaxChainIdBytes = 256;
// The hello layer already refuses anything longer than kMaxHelloBytes, so the transcript uses that same bound
// rather than a second number that could drift away from it.

bool put_length_prefixed(std::string& out, std::string_view bytes, std::size_t limit) {
    if (bytes.size() > limit || bytes.size() > 0xffffffffull) return false;
    put_u32be(out, static_cast<std::uint32_t>(bytes.size()));
    out.append(bytes.data(), bytes.size());
    return true;
}

int nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <std::size_t N>
bool parse_hex_n(std::string_view text, std::array<std::uint8_t, N>& out) {
    if (text.size() != N * 2) return false;
    for (std::size_t i = 0; i < N; ++i) {
        const int hi = nib(text[2 * i]);
        const int lo = nib(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

std::string auth_transcript(const Hello& one, std::string_view one_hello_bytes,
                            const Hello& two, std::string_view two_hello_bytes) {
    // Order by nonce, not by who called: the transcript must come out the same on both sides even if one of them
    // passed the arguments the other way round. Sorting on the nonce removes that dependency entirely, and the
    // two nonces differ, so reflection (bouncing our own message back) still produces different bytes.
    const bool one_first = std::lexicographical_compare(one.nonce.begin(), one.nonce.end(),
                                                       two.nonce.begin(), two.nonce.end());
    const Hello& first = one_first ? one : two;
    const std::string_view first_hello_bytes = one_first ? one_hello_bytes : two_hello_bytes;
    const Hello& second = one_first ? two : one;
    const std::string_view second_hello_bytes = one_first ? two_hello_bytes : one_hello_bytes;

    std::string out;
    out.reserve(kAuthDomain.size() + 2 * (4 + 4 + 32) + first.chain_id.size() + second.chain_id.size() + 32 + 32 +
                first_hello_bytes.size() + second_hello_bytes.size() + 8);
    out.append(kAuthDomain.data(), kAuthDomain.size());
    // The chain identity of BOTH sides, so a signature cannot be moved to another chain whichever side differs.
    // (v9.2: before this only the lower-nonce side was covered by the fields; the other side relied on its raw
    // hello bytes alone, and a test that changed the other side's fields passed or failed by coin flip.)
    for (const Hello* h : {&first, &second}) {
        put_u32be(out, h->protocol_version);
        if (!put_length_prefixed(out, h->chain_id, kMaxChainIdBytes)) return {};
        put_bytes(out, h->genesis_commitment);
    }
    put_bytes(out, first.nonce);
    put_bytes(out, second.nonce);
    if (!put_length_prefixed(out, first_hello_bytes, kMaxHelloBytes)) return {};
    if (!put_length_prefixed(out, second_hello_bytes, kMaxHelloBytes)) return {};
    return out;
}

std::string encode_auth(const AuthMessage& message) {
    std::string out;
    out += "pubkey=" + to_hex(message.pubkey) + "\n";
    out += "signature=" + to_hex(message.signature) + "\n";
    return out;
}

bool decode_auth(std::string_view text, AuthMessage& out, std::string* why) {
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (text.empty()) return fail("the auth message is empty");
    if (text.size() > 512) return fail("the auth message is too large");
    bool have_pubkey = false, have_signature = false;
    AuthMessage parsed;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) return fail("the auth message must end with a newline");
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return fail("every line must be key=value");
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        if (key == "pubkey") {
            if (have_pubkey) return fail("duplicate pubkey");
            if (!parse_hex_n(value, parsed.pubkey)) return fail("pubkey must be 64 hex characters");
            have_pubkey = true;
        } else if (key == "signature") {
            if (have_signature) return fail("duplicate signature");
            if (!parse_hex_n(value, parsed.signature)) return fail("signature must be 128 hex characters");
            have_signature = true;
        } else {
            return fail("unknown field \"" + std::string(key) + "\"");
        }
    }
    if (!have_pubkey || !have_signature) return fail("a required field is missing");
    out = parsed;
    return true;
}

bool sign_auth(AuthMessage& message, const PrivKey& private_seed, std::string_view transcript) {
    if (transcript.empty()) return false;
    // Ed25519 treats an all-zero 32-byte seed as a valid key, so an uninitialised PrivKey{} would happily sign.
    // A node with no identity must not produce one.
    bool any = false;
    for (std::uint8_t b : private_seed)
        if (b != 0) any = true;
    if (!any) return false;
    if (!public_from_private(private_seed, message.pubkey)) return false;
    return sign(private_seed, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(transcript.data()),
                                                          transcript.size()),
                message.signature);
}

bool auth_ok(const AuthMessage& message, std::string_view transcript) {
    if (transcript.empty()) return false;
    // An all-zero public key is "no identity", not a key that can prove anything.
    bool any = false;
    for (std::uint8_t b : message.pubkey)
        if (b != 0) any = true;
    if (!any) return false;
    return verify(message.pubkey,
                  std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(transcript.data()),
                                                transcript.size()),
                  message.signature);
}

}  // namespace taz
