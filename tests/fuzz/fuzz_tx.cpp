// libFuzzer target: arbitrary transaction field combinations must never crash the shape rules, the
// signature payload builder or the txid. No key is available, so signatures are junk on purpose; the
// point is the encoding and the arithmetic, not the cryptography.
#include "taz/tx.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
struct Cursor {
    const std::uint8_t* p; std::size_t n; std::size_t i{0};
    std::uint8_t u8() { return i < n ? p[i++] : 0; }
    std::uint64_t u64() { std::uint64_t v = 0; for (int k = 0; k < 8; ++k) v = (v << 8) | u8(); return v; }
    template <class A> void fill(A& a) { for (auto& b : a) b = u8(); }
};
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 8) return 0;
    Cursor c{data, size};
    taz::Transaction tx;
    tx.protocol_version = static_cast<std::uint32_t>(c.u8() % 4);
    tx.kind = static_cast<taz::TxKind>(c.u8() % 6);
    tx.amount = c.u64();
    tx.fee = (c.u8() & 1) ? taz::FEE_TRANSFER_V1 : c.u64();
    tx.nonce = c.u64();
    tx.expiry_height = (c.u8() & 1) ? 0 : c.u64();
    tx.daily_limit = c.u64();
    tx.hold_threshold = c.u64();
    tx.hold_blocks = c.u64();
    tx.guardian_threshold = c.u64();
    c.fill(tx.from); c.fill(tx.to); c.fill(tx.ref); c.fill(tx.new_owner); c.fill(tx.veto);
    c.fill(tx.pubkey); c.fill(tx.signature);
    const std::size_t guardians = c.u8() % 8;
    for (std::size_t i = 0; i < guardians; ++i) { taz::Address a{}; c.fill(a); tx.guardians.push_back(a); }
    if (c.u8() & 1) tx.chain_id = (c.u8() & 1) ? "tazvera-local-prototype-v1" : "x";

    (void)taz::validate_tx_shape(tx);
    const auto bytes = tx.signable_bytes();
    const taz::Hash32 id = tx.txid();
    if (bytes.empty() && id == taz::Hash32{}) __builtin_trap();   // signable bytes are never empty
    return 0;
}
