// Builds a tiny throw-away chain with fresh random keys and writes it as JSON for the explorer.
// Usage: taz_export_demo [output.json] [--safety]
//   --safety  also exercises the spending limit, a held transfer that is cancelled, a held transfer
//             that is released, and a loosening of the rules that has to wait.
#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <cstring>
#include <fstream>
#include <iostream>

using namespace taz;
namespace {
struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
bool new_key(Key& k) { if (!generate_keypair(k.sk, k.pk)) return false; k.addr = address_from_pubkey(k.pk); return true; }
bool signit(const Key& k, Transaction& tx) { tx.pubkey = k.pk; tx.from = k.addr; return sign(k.sk, tx.signable_bytes(), tx.signature); }
Transaction transfer(const Key& from, const Address& to, Amount amount, std::uint64_t nonce) {
    Transaction tx; tx.to = to; tx.amount = amount; tx.nonce = nonce; tx.pubkey = from.pk; tx.from = from.addr;
    signit(from, tx); return tx;
}
Transaction set_safety(const Key& k, Amount limit, Amount thr, std::uint64_t hold, std::uint64_t nonce) {
    Transaction tx; tx.kind = TxKind::SetSafety; tx.daily_limit = limit; tx.hold_threshold = thr; tx.hold_blocks = hold; tx.nonce = nonce;
    signit(k, tx); return tx;
}
Transaction cancel(const Key& k, const Hash32& ref, std::uint64_t nonce) {
    Transaction tx; tx.kind = TxKind::CancelHeld; tx.ref = ref; tx.nonce = nonce; signit(k, tx); return tx;
}
}
int main(int argc, char** argv) {
    std::string path = "tazvera_demo_chain.json"; bool with_safety = false;
    for (int i = 1; i < argc; ++i) { if (std::strcmp(argv[i], "--safety") == 0) with_safety = true; else path = argv[i]; }
    Key alice, bob;
    if (!new_key(alice) || !new_key(bob)) { std::cerr << "key generation failed\n"; return 1; }
    Chain chain;
    if (!chain.init_genesis(alice.addr, 100000000, bob.addr, 0)) return 1;
    std::int64_t ts = 1700000000; auto next = [&ts]() { return ts += 100; };
    bool ok = true;
    if (!with_safety) {
        ok = chain.append_block({transfer(alice, bob.addr, 5000000, 0)}, next()).ok
          && chain.append_block({}, next()).ok
          && chain.append_block({transfer(bob, alice.addr, 1200000, 0)}, next()).ok
          && chain.append_block({transfer(alice, bob.addr, 250000, 1)}, next()).ok;
    } else {
        // 1: Alice protects her account: at most 20,000,000 per window; anything >= 3,000,000 waits 5 blocks.
        ok = chain.append_block({set_safety(alice, 20000000, 3000000, 5, 0)}, next()).ok;
        // 2: a small payment arrives at once.
        ok = ok && chain.append_block({transfer(alice, bob.addr, 1000000, 1)}, next()).ok;
        // 3: a big payment is held. 4: Alice notices it was a mistake and cancels it.
        const Transaction mistake = transfer(alice, bob.addr, 6000000, 2);
        ok = ok && chain.append_block({mistake}, next()).ok;
        ok = ok && chain.append_block({cancel(alice, mistake.txid(), 3)}, next()).ok;
        // 5: another big payment is held and, nobody cancelling it, arrives 5 blocks later.
        ok = ok && chain.append_block({transfer(alice, bob.addr, 4000000, 4)}, next()).ok;
        for (int i = 0; ok && i < 5; ++i) ok = chain.append_block({}, next()).ok;
        // Bob can spend what arrived.
        ok = ok && chain.append_block({transfer(bob, alice.addr, 500000, 0)}, next()).ok;
        // A raised limit is only a request: it waits SAFETY_DELAY_BLOCKS before it counts.
        ok = ok && chain.append_block({set_safety(alice, 50000000, 3000000, 5, 5)}, next()).ok;
    }
    secure_wipe(alice.sk); secure_wipe(bob.sk); // the demo keys are never saved
    if (!ok) { std::cerr << "demo chain failed\n"; return 1; }
    std::ofstream out(path, std::ios::binary);
    out << to_json(chain);
    if (!out) { std::cerr << "could not write " << path << "\n"; return 1; }
    std::cout << "Wrote " << path << " (" << chain.blocks().size() << " blocks)\n"
              << "Alice: " << to_hex(alice.addr) << "\nBob:   " << to_hex(bob.addr) << "\n";
    return 0;
}
