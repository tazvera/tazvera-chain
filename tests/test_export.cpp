#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k; }
static Transaction tx_of(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx; tx.from = f.addr; tx.to = to; tx.amount = a; tx.nonce = n; tx.pubkey = f.pk;
    CHECK(sign(f.sk, tx.signable_bytes(), tx.signature)); return tx;
}
static std::size_t count(const std::string& s, const std::string& needle) {
    std::size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); } return n;
}
int main() {
    Key a = new_key(), b = new_key(), c = new_key();
    Chain chain; CHECK(chain.init_genesis(a.addr, 50000000, b.addr, 0));
    CHECK(chain.genesis_balances().size() == 1); // zero balance is not listed
    CHECK(chain.append_block({tx_of(a, b.addr, 1000000, 0), tx_of(a, c.addr, 20, 1)}, 1).ok);
    CHECK(chain.append_block({}, 2).ok);
    CHECK(chain.append_block({tx_of(b, c.addr, 300, 0), tx_of(a, b.addr, 5, 2)}, 3).ok);

    // the statement must agree with the real state for every address, including an unknown one
    for (const Address& who : {a.addr, b.addr, c.addr, Address{9}}) {
        const auto s = wallet_statement(chain, who);
        CHECK(s.balance == chain.state().balance(who));
        CHECK(s.nonce == chain.state().nonce(who));
    }
    const auto sa = wallet_statement(chain, a.addr);
    CHECK(sa.nonce == 3); CHECK(sa.total_fees == 3 * FEE_TRANSFER_V1); CHECK(sa.total_sent == 1000025);
    CHECK(sa.entries.size() == 3 && sa.entries[0].kind == EntryKind::Sent && sa.entries[0].height == 1);
    const auto sc = wallet_statement(chain, c.addr);
    CHECK(sc.balance == 320 && sc.entries.size() == 2 && sc.entries[0].kind == EntryKind::Received && sc.total_fees == 0);
    CHECK(wallet_statement(chain, Address{9}).entries.empty());
    // supply is conserved: all balances + burned == genesis
    CHECK(chain.state().circulating() + chain.state().burned_total() == 50000000);

    const std::string j = to_json(chain);
    CHECK(count(j, "\"txid\"") == 4); CHECK(count(j, "\"state_root\"") == 4);
    CHECK(j.find("tazvera-chain-export-v1") != std::string::npos);
    CHECK(j.find(to_hex(chain.blocks().back().hash())) != std::string::npos);
    // numbers that exceed 2^53 stay exact because they are strings
    Chain big; CHECK(big.init_genesis(a.addr, 18446744073709551000ULL, b.addr, 0));
    CHECK(to_json(big).find("\"18446744073709551000\"") != std::string::npos);
    // output is brace/bracket balanced (cheap well-formedness check; no raw control chars)
    long depth = 0; for (char ch : j) { if (ch=='{'||ch=='[') ++depth; if (ch=='}'||ch==']') --depth; CHECK(static_cast<unsigned char>(ch) >= 0x20 || ch=='\n'); }
    CHECK(depth == 0);

    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "Export/statement tests passed.\n"; return EXIT_SUCCESS;
}
