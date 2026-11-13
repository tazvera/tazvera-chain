// Builds a tiny throw-away chain with fresh random keys and writes it as JSON for the explorer.
// Usage: taz_export_demo [output.json]
#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <fstream>
#include <iostream>

using namespace taz;
namespace {
struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
bool new_key(Key& k) { if (!generate_keypair(k.sk, k.pk)) return false; k.addr = address_from_pubkey(k.pk); return true; }
bool transfer(Chain& chain, const Key& from, const Address& to, Amount amount, std::uint64_t nonce, std::int64_t ts) {
    Transaction tx; tx.from = from.addr; tx.to = to; tx.amount = amount; tx.nonce = nonce; tx.pubkey = from.pk;
    if (!sign(from.sk, tx.signable_bytes(), tx.signature)) return false;
    return chain.append_block({tx}, ts).ok;
}
}
int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "tazvera_demo_chain.json";
    Key alice, bob;
    if (!new_key(alice) || !new_key(bob)) { std::cerr << "key generation failed\n"; return 1; }
    Chain chain;
    if (!chain.init_genesis(alice.addr, 100000000, bob.addr, 0)) return 1;
    bool ok = transfer(chain, alice, bob.addr, 5000000, 0, 1700000100)
           && chain.append_block({}, 1700000200).ok
           && transfer(chain, bob, alice.addr, 1200000, 0, 1700000300)
           && transfer(chain, alice, bob.addr, 250000, 1, 1700000400);
    secure_wipe(alice.sk); secure_wipe(bob.sk); // the demo keys are never saved
    if (!ok) { std::cerr << "demo chain failed\n"; return 1; }
    std::ofstream out(path, std::ios::binary);
    out << to_json(chain);
    if (!out) { std::cerr << "could not write " << path << "\n"; return 1; }
    std::cout << "Wrote " << path << " (" << chain.blocks().size() << " blocks)\n"
              << "Alice: " << to_hex(alice.addr) << "\nBob:   " << to_hex(bob.addr) << "\n";
    return 0;
}
