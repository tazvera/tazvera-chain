#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k; CHECK(generate_keypair(k.sk, k.pk)); k.addr = address_from_pubkey(k.pk); return k;
}
static void resign(Transaction& tx, const PrivKey& sk) {
    CHECK(sign(sk, tx.signable_bytes(), tx.signature));
}
static Transaction make_tx(const Key& from, const Address& to, Amount amount, std::uint64_t nonce) {
    Transaction tx; tx.from = from.addr; tx.to = to; tx.amount = amount; tx.fee = FEE_TRANSFER_V1;
    tx.nonce = nonce; tx.pubkey = from.pk; resign(tx, from.sk); return tx;
}
static constexpr Amount INITIAL = 50'000'000;

static bool same_tip(const Chain& a, const Chain& b) {
    if (a.height() != b.height()) return false;
    if (a.state().state_root() != b.state().state_root()) return false;
    if (a.blocks().size() != b.blocks().size()) return false;
    for (std::size_t i = 0; i < a.blocks().size(); ++i) {
        if (a.blocks()[i].hash() != b.blocks()[i].hash()) return false;
        if (a.blocks()[i].state_root != b.blocks()[i].state_root) return false;
    }
    return true;
}

int main() {
    Key alice = new_key(), bob = new_key();
    Chain chain;
    CHECK(chain.init_genesis(alice.addr, INITIAL, bob.addr, 1'000'000));
    CHECK(chain.append_block({make_tx(alice, bob.addr, 1000, 0)}, 10).ok);
    CHECK(chain.append_block({make_tx(alice, bob.addr, 2000, 1), make_tx(alice, bob.addr, 3000, 2)}, 20).ok);

    const std::string path = "taz_persist_roundtrip.json";
    std::string err;
    CHECK(save_chain_json(chain, path, &err));

    Chain loaded;
    CHECK(load_chain_json(path, loaded, &err));
    CHECK(same_tip(chain, loaded));
    CHECK(loaded.state().balance(alice.addr) == chain.state().balance(alice.addr));
    CHECK(loaded.state().balance(bob.addr) == chain.state().balance(bob.addr));
    CHECK(loaded.state().burned_total() == chain.state().burned_total());

    // in-memory round-trip
    Chain loaded2;
    CHECK(load_chain_json_string(to_json(chain), loaded2, &err));
    CHECK(same_tip(chain, loaded2));

    // trailing garbage
    {
        Chain x;
        CHECK(!load_chain_json_string(to_json(chain) + "x", x, &err));
        CHECK(err.find("trailing") != std::string::npos || err.find("garbage") != std::string::npos);
    }

    // truncated
    {
        std::string j = to_json(chain);
        Chain x;
        CHECK(!load_chain_json_string(j.substr(0, j.size() / 2), x, &err));
    }

    // wrong chain id
    {
        std::string j = to_json(chain);
        auto pos = j.find(std::string(CHAIN_ID));
        CHECK(pos != std::string::npos);
        j.replace(pos, CHAIN_ID.size(), "evil-chain-id-not-tazvera");
        Chain x;
        CHECK(!load_chain_json_string(j, x, &err));
    }

    // tamper state_root in tip block
    {
        std::string j = to_json(chain);
        // flip a hex digit inside the last state_root occurrence
        auto pos = j.rfind("\"state_root\"");
        CHECK(pos != std::string::npos);
        pos = j.find('"', pos + 12);
        CHECK(pos != std::string::npos);
        // value starts after ": "
        pos = j.find('"', pos + 1);
        CHECK(pos != std::string::npos && pos + 3 < j.size());
        char& ch = j[pos + 3];
        ch = (ch == '0') ? '1' : '0';
        Chain x;
        CHECK(!load_chain_json_string(j, x, &err));
    }

    // tamper signature → accept_block fails
    {
        std::string j = to_json(chain);
        auto pos = j.rfind("\"signature\"");
        CHECK(pos != std::string::npos);
        pos = j.find('"', j.find(':', pos));
        CHECK(pos != std::string::npos && pos + 5 < j.size());
        j[pos + 3] = (j[pos + 3] == 'a') ? 'b' : 'a';
        Chain x;
        CHECK(!load_chain_json_string(j, x, &err));
    }

    // empty / missing file
    {
        Chain x;
        CHECK(!load_chain_json("no_such_tazvera_chain_file.json", x, &err));
    }

    // bad path save
    CHECK(!save_chain_json(chain, "/nonexistent_dir_tazvera/x.json", &err));

    // no private keys in export
    {
        std::string j = to_json(chain);
        CHECK(j.find("\"signature\"") != std::string::npos);
        CHECK(j.find("private") == std::string::npos);
        CHECK(j.find("seed") == std::string::npos);
        CHECK(j.find(to_hex(alice.sk)) == std::string::npos);
    }

    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "persist tests passed (save/load/replay). Not a security audit.\n";
    return 0;
}
