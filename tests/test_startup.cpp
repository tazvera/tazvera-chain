// v9.0: mandatory genesis pin at startup.
//
// The chain file is not evidence. Every case below is about the same question: when a program starts, does it
// load whatever it is handed, or only the chain its operator named out of band? The startup path must fail
// closed, and it must never take the commitment from the file it is checking.
#include "net_ctx.hpp"
#include "taz/startup.hpp"
#include "taz/export.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace taz;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) { std::cerr << "keygen failed\n"; std::exit(2); }
    k.addr = address_from_pubkey(k.pk);
    return k;
}
static void resign(Transaction& tx, const Key& k) { tx.network_id = taztest::g_net; if (!sign(k.sk, tx.signable_bytes(), tx.signature)) std::exit(2); }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx; tx.from = f.addr; tx.nonce = n; tx.pubkey = f.pk; tx.to = to; tx.amount = a;
    resign(tx, f); return tx;
}
static std::int64_t ts = 500;

static std::string tmpdir() {
    const std::string dir = (fs::temp_directory_path() / "taz-startup-tests").string();
    std::error_code ec; fs::create_directories(dir, ec);
    return dir;
}
static std::string write_file(const std::string& name, const std::string& text) {
    const std::string path = tmpdir() + "/" + name;
    std::ofstream out(path, std::ios::binary); out << text;
    return path;
}

// 1. the right pin loads the chain
static void s01_correct_pin_loads() {
    Key o = new_key(), g = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0)); taztest::use(c);
    CHECK(c.append_block({transfer(o, g.addr, 10, 0)}, ts++).ok);
    std::string err;
    const std::string path = write_file("s01.json", to_json(c));
    StartupConfig cfg{path, c.genesis_commitment()};
    Chain loaded;
    const StartupResult r = load_chain_at_startup(cfg, loaded, &err);
    CHECK(r.ok);
    if (!r.ok) { std::cerr << "  " << err << "\n"; return; }
    CHECK(loaded.height() == c.height());
    CHECK(loaded.genesis_commitment() == c.genesis_commitment());
}

// 2. a pin for a different chain is refused
static void s02_wrong_pin_refused() {
    Key o = new_key(), g = new_key(), other = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000000, g.addr, 0)); taztest::use(c);
    Chain elsewhere; CHECK(elsewhere.init_genesis(other.addr, 1, g.addr, 0)); taztest::use(elsewhere);
    std::string err;
    const std::string path = write_file("s02.json", to_json(c));
    StartupConfig cfg{path, elsewhere.genesis_commitment()};
    Chain loaded;
    const StartupResult r = load_chain_at_startup(cfg, loaded, &err);
    CHECK(!r.ok);
    CHECK(loaded.blocks().empty());          // a refused startup leaves nothing half-loaded
}

// 3. no pin at all: fail closed, before touching the file
static void s03_missing_pin_fails_closed() {
    Key o = new_key(), g = new_key();
    Chain c; CHECK(c.init_genesis(o.addr, 1000, g.addr, 0)); taztest::use(c);
    const std::string path = write_file("s03.json", to_json(c));
    StartupConfig cfg{path, std::nullopt};
    std::string err;
    Chain loaded;
    const StartupResult r = load_chain_at_startup(cfg, loaded, &err);
    CHECK(!r.ok);
    CHECK(err.find("trusted genesis") != std::string::npos);
    CHECK(loaded.blocks().empty());
}

// 4. the attacker rebuilds a chain and fixes its own internal commitment — the pin still refuses it
static void s04_selfconsistent_forgery_refused() {
    Key mine_o = new_key(), g = new_key(), their_o = new_key();
    Chain mine;  CHECK(mine.init_genesis(mine_o.addr, 1000000, g.addr, 0)); taztest::use(mine);
    Chain forged; CHECK(forged.init_genesis(their_o.addr, 999999999, g.addr, 0)); taztest::use(forged);
    CHECK(forged.append_block({transfer(their_o, g.addr, 5, 0)}, ts++).ok);
    const std::string forged_text = to_json(forged);
    // the forged file is internally consistent: its own commitment field matches its own blocks
    Chain self; std::string err;
    CHECK(load_chain_json_text(forged_text, self, &err));
    CHECK(self.genesis_commitment() == forged.genesis_commitment());
    // and it is still refused by the pin of the real chain
    const std::string path = write_file("s04.json", forged_text);
    StartupConfig cfg{path, mine.genesis_commitment()};
    Chain loaded;
    const StartupResult r = load_chain_at_startup(cfg, loaded, &err);
    CHECK(!r.ok);
    CHECK(loaded.blocks().empty());
}

// 5. files written before v9 keep loading, and a producer chain loads with its own pin
static void s05_compatibility() {
    Key o = new_key(), g = new_key(), p1 = new_key();
    // a plain chain, exported the way v8.11 did (no producer fields anywhere)
    Chain plain; CHECK(plain.init_genesis(o.addr, 1000000, g.addr, 0)); taztest::use(plain);
    CHECK(plain.append_block({transfer(o, g.addr, 10, 0)}, ts++).ok);
    const std::string plain_text = to_json(plain);
    CHECK(plain_text.find("producer") == std::string::npos);
    CHECK(plain_text.find("genesis_producers") == std::string::npos);
    std::string err;
    StartupConfig plain_cfg{write_file("s05a.json", plain_text), plain.genesis_commitment()};
    Chain loaded_plain;
    CHECK(load_chain_at_startup(plain_cfg, loaded_plain, &err).ok);
    CHECK(loaded_plain.height() == plain.height());

    // a v9 chain with a producer set
    Chain prod; CHECK(prod.init_genesis(o.addr, 1000000, g.addr, 0, {p1.addr})); taztest::use(prod);
    CHECK(prod.append_block_signed({transfer(o, g.addr, 10, 0)}, ts++, p1.sk, p1.pk).ok);
    StartupConfig prod_cfg{write_file("s05b.json", to_json(prod)), prod.genesis_commitment()};
    Chain loaded_prod;
    CHECK(load_chain_at_startup(prod_cfg, loaded_prod, &err).ok);
    CHECK(loaded_prod.producers().size() == 1);
    CHECK(loaded_prod.blocks()[1].producer == p1.addr);
}

int main() {
    s01_correct_pin_loads();
    s02_wrong_pin_refused();
    s03_missing_pin_fails_closed();
    s04_selfconsistent_forgery_refused();
    s05_compatibility();
    std::error_code ec; fs::remove_all(tmpdir(), ec);
    if (failures == 0) { std::cout << "startup tests: all passed\n"; return 0; }
    std::cerr << "startup tests: " << failures << " failure(s)\n";
    return 1;
}
