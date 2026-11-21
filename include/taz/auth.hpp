#pragma once
#include "taz/hello.hpp"

namespace taz {

// v9.1: proving which key is speaking, bound to THIS connection.
//
// A hello alone cannot do it: a recorded handshake could be replayed by anyone. So each side sends a fresh
// 32-byte nonce in its hello, and then signs a transcript that contains BOTH nonces and BOTH hellos. Replaying
// an old message fails (the nonce is new), and bouncing a peer's own message back fails (the two nonces differ).
//
// What it proves: this key produced this transcript on this connection. It does not prove the peer is honest,
// trusted or allowed to do anything. That is authorisation, and it is a different milestone.

// The domain string is part of the signed bytes, so its length matters. It is 15, not 16:
// "TAZVERA" (7) + "-" (1) + "AUTH" (4) + "-" (1) + "V1" (2).
inline constexpr std::string_view kAuthDomain = "TAZVERA-AUTH-V1";
static_assert(kAuthDomain.size() == 15, "the domain length is part of the signed bytes");

// Canonical encoding: no separators, every variable-length field carries a u32 big-endian length, every integer
// is fixed width big-endian. Length prefixes are what stop "ab"+"c" and "a"+"bc" from producing the same bytes.
// The hello arguments are the exact bytes that travelled on the wire, not a re-encoding of them.
std::string auth_transcript(const Hello& first, std::string_view first_hello_bytes,
                            const Hello& second, std::string_view second_hello_bytes);

struct AuthMessage {
    PubKey pubkey{};
    Signature signature{};
};

// Wire form: two lines, "pubkey=<64 hex>" and "signature=<128 hex>". Strict on the way in.
std::string encode_auth(const AuthMessage& message);
bool decode_auth(std::string_view text, AuthMessage& out, std::string* why = nullptr);

bool sign_auth(AuthMessage& message, const PrivKey& private_seed, std::string_view transcript);
bool auth_ok(const AuthMessage& message, std::string_view transcript);

}  // namespace taz
