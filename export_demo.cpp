// Builds a tiny throw-away chain with fresh random keys and writes it as JSON for the explorer.
// Usage: taz_export_demo [output.json] [--safety | --recovery]
//   --safety  also exercises the spending limit, a held transfer that is cancelled, a held transfer
//             that is released, and a loosening of the rules that has to wait.
//   --recovery  Alice names three trusted people; a first recovery attempt is cancelled by her; later she
//             "loses" her key and two of the three bring her funds to a new key.
#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <algorithm>
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
Transaction set_guardians(const Key& k, std::vector<Address> list, std::uint64_t thr, std::uint64_t nonce) {
    std::sort(list.begin(), list.end());
    Transaction tx; tx.kind = TxKind::SetGuardians; tx.guardians = std::move(list); tx.guardian_threshold = thr; tx.nonce = nonce; signit(k, tx); return tx;
}
Transaction vote(const Key& g, const Address& account, const Address& new_owner, std::uint64_t nonce) {
    Transaction tx; tx.kind = TxKind::RecoveryVote; tx.to = account; tx.new_owner = new_owner; tx.nonce = nonce; signit(g, tx); return tx;
}
Transaction set_guardians_veto(const Key& k, std::vector<Address> list, std::uint64_t thr, const Address& veto, std::uint64_t nonce) {
    Transaction tx = set_guardians(k, std::move(list), thr, nonce); // (signed once without the veto; sign again with it)
    tx.veto = veto; signit(k, tx); return tx;
}
Transaction veto_cancel(const Key& veto_key, const Address& account, std::uint64_t nonce) {
    Transaction tx; tx.kind = TxKind::RecoveryCancel; tx.to = account; tx.nonce = nonce; signit(veto_key, tx); return tx;
}
Transaction cancel_recovery(const Key& k, std::uint64_t nonce) {
    Transaction tx; tx.kind = TxKind::RecoveryCancel; tx.nonce = nonce; signit(k, tx); return tx;
}
}
int main(int argc, char** argv) {
    std::string path = "tazvera_demo_chain.json"; bool with_safety = false, with_recovery = false;
    for (int i = 1; i < argc; ++i) { if (std::strcmp(argv[i], "--safety") == 0) with_safety = true; else if (std::strcmp(argv[i], "--recovery") == 0) with_recovery = true; else path = argv[i]; }
    Key alice, bob, charlie, dave, alice2, eve, vault;
    if (!new_key(alice) || !new_key(bob) || !new_key(charlie) || !new_key(dave) || !new_key(alice2) || !new_key(eve) || !new_key(vault)) { std::cerr << "key generation failed\n"; return 1; }
    Chain chain;
    if (!chain.init_genesis(alice.addr, 100000000, bob.addr, 0)) return 1;
    std::int64_t ts = 1700000000; auto next = [&ts]() { return ts += 100; };
    bool ok = true;
    if (with_recovery) {
        // 1: Alice funds Bob, Charlie, Dave (fee payers) and the offline vault (veto key).
        ok = chain.append_block({transfer(alice, bob.addr, 2000000, 0), transfer(alice, charlie.addr, 2000000, 1),
                                 transfer(alice, dave.addr, 2000000, 2), transfer(alice, vault.addr, 1000000, 3)}, next()).ok;
        // 2: Alice registers 2-of-3 guardians + independent veto key (takes effect after SAFETY_DELAY).
        ok = ok && chain.append_block({set_guardians_veto(alice, {bob.addr, charlie.addr, dave.addr}, 2, vault.addr, 4)}, next()).ok;
        for (std::uint64_t i = 0; ok && i < SAFETY_DELAY_BLOCKS; ++i) ok = chain.append_block({}, next()).ok;
        // 3: Hostile first attempt starts (one vote only). Veto clears votes while recovery is not yet active.
        //    Under R2, a veto against an *active* recovery only postpones once; clearing early votes is a full cancel.
        ok = ok && chain.append_block({vote(bob, alice.addr, eve.addr, 0)}, next()).ok;
        ok = ok && chain.append_block({veto_cancel(vault, alice.addr, 0)}, next()).ok;
        // 4: Alice has lost her everyday key. Two guardians vote for her new key; nobody vetoes.
        ok = ok && chain.append_block({vote(dave, alice.addr, alice2.addr, 0)}, next()).ok;
        ok = ok && chain.append_block({vote(bob, alice.addr, alice2.addr, 1)}, next()).ok;
        for (std::uint64_t i = 0; ok && i < RECOVERY_DELAY_BLOCKS; ++i) ok = chain.append_block({}, next()).ok;
        // 5: New key can spend.
        ok = ok && chain.append_block({transfer(alice2, bob.addr, 1500000, 0)}, next()).ok;
    } else if (!with_safety) {
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
    for (Key* k : {&alice, &bob, &charlie, &dave, &alice2, &eve, &vault}) secure_wipe(k->sk); // the demo keys are never saved
    if (!ok) { std::cerr << "demo chain failed\n"; return 1; }
    std::ofstream out(path, std::ios::binary);
    out << to_json(chain);
    if (!out) { std::cerr << "could not write " << path << "\n"; return 1; }
    std::cout << "Wrote " << path << " (" << chain.blocks().size() << " blocks)\n"
              << "Alice: " << to_hex(alice.addr) << "\nBob:   " << to_hex(bob.addr) << "\n";
    if (with_recovery) std::cout << "Charlie: " << to_hex(charlie.addr) << "\nDave:  " << to_hex(dave.addr) << "\nVault (veto key): " << to_hex(vault.addr) << "\nAlice (new key): " << to_hex(alice2.addr) << "\n";
    return 0;
}
