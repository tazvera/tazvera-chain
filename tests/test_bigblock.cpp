// v9.1: a block full of REAL transactions must survive the wire.
//
// The existing worst-case test builds a block from a deliberately invalid transaction and only measures how many
// bytes the writer and parser can handle. That proves the limits, not that a valid multi-transaction block can be
// built, sent, parsed and accepted — which is what a node actually has to do. This test uses nothing but valid
// transactions: correct nonces, correct fees, real signatures, and a balance that covers all of them.
#include "net_ctx.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/net.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) std::exit(2);
    k.addr = address_from_pubkey(k.pk);
    return k;
}

static Transaction transfer(const Key& from, const Address& to, Amount amount, std::uint64_t nonce) {
    Transaction tx;
    tx.from = from.addr; tx.to = to; tx.amount = amount; tx.nonce = nonce;
    tx.fee = FEE_TRANSFER_V1; tx.pubkey = from.pk;
    tx.network_id = taztest::g_net;
    if (!sign(from.sk, tx.signable_bytes(), tx.signature)) std::exit(2);
    return tx;
}

int main() {
    // 5000, not 10000: the point is a block far beyond one old 64 KiB message, and half the maximum keeps CI quick
    // without weakening the claim. MAX_BLOCK_TXS is checked so this stays honest if the constant ever changes.
    const std::size_t kCount = 5000;
    CHECK(kCount <= MAX_BLOCK_TXS);

    Key owner = new_key(), receiver = new_key();
    const Amount needed = static_cast<Amount>(kCount) * (1 + FEE_TRANSFER_V1);
    Chain chain;
    CHECK(chain.init_genesis(owner.addr, needed + 1000, receiver.addr, 0)); taztest::use(chain);

    std::vector<Transaction> txs;
    txs.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) txs.push_back(transfer(owner, receiver.addr, 1, i));

    const ChainResult appended = chain.append_block(txs, 1000);
    if (!appended.ok) std::cerr << "  append refused: " << appended.error << "\n";
    CHECK(appended.ok);
    CHECK(chain.height() == 1);
    CHECK(chain.state().balance(receiver.addr) == static_cast<Amount>(kCount));

    // what the node does: write it, and check it fits in one message
    const std::string text = block_to_json(chain.blocks().back());
    std::cout << "  valid block: " << kCount << " transactions = " << text.size() << " bytes (limit "
              << kMaxPayloadBytes << ")\n";
    CHECK(text.size() <= kMaxPayloadBytes);

    // what a peer does: read it back and accept it on its own copy of the chain
    Block back;
    std::string why;
    const bool parsed = parse_block_text(text, back, &why);
    if (!parsed) std::cerr << "  parse refused: " << why << "\n";
    CHECK(parsed);
    CHECK(back.hash() == chain.blocks().back().hash());
    CHECK(back.transactions.size() == kCount);

    Chain peer;
    CHECK(peer.init_genesis(owner.addr, needed + 1000, receiver.addr, 0)); taztest::use(peer);
    const ChainResult accepted = peer.accept_block(back);
    if (!accepted.ok) std::cerr << "  accept refused: " << accepted.error << "\n";
    CHECK(accepted.ok);
    CHECK(peer.state().state_root() == chain.state().state_root());
    CHECK(peer.blocks().back().hash() == chain.blocks().back().hash());

    if (failures == 0) { std::cout << "big-block tests: all passed\n"; return 0; }
    std::cerr << "big-block tests: " << failures << " failure(s)\n";
    return 1;
}
