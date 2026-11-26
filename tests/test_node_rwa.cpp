// taznode: the two RWA commands, --register-asset <kind>:<doc_hash> and --transfer-asset <asset_id>:<to>.
//
// Process-level: the real taznode binary is run, a block is made and saved, and the chain file is read back.
// Both commands work like --transfer: they need --chain, --expect-genesis and --key-file, make exactly one block and stop.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef TAZ_NODE_BINARY
#error "TAZ_NODE_BINARY must be defined (see CMakeLists.txt)"
#endif

namespace fs = std::filesystem;
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static std::string tmpdir() {
    const std::string dir = (fs::temp_directory_path() / "taz-node-rwa-tests").string();
    std::error_code ec; fs::create_directories(dir, ec);
    return dir;
}
static std::string slurp(const std::string& path) { std::ifstream in(path, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static void write_text(const std::string& path, const std::string& text) { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text; }
static std::string q(const std::string& s) { return "\"" + s + "\""; }
static int run(const std::string& cmd) {
#ifdef _WIN32
    const std::string full = "\"" + cmd + "\"";
#else
    const std::string& full = cmd;
#endif
    const int rc = std::system(full.c_str());
    if (rc == -1) return -1;
#ifdef _WIN32
    return rc;
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}
static std::string hexs(std::uint8_t seed, std::size_t n) { std::string s; for (std::size_t i = 0; i < n; ++i) { char b[3]; std::snprintf(b, sizeof b, "%02x", static_cast<unsigned>((seed + i) & 0xff)); s += b; } return s; }

struct Person { PrivKey sk{}; PubKey pk{}; Address addr{}; std::string key_file; };
static Person person(const std::string& name) {
    Person p; if (!generate_keypair(p.sk, p.pk)) std::exit(2);
    p.addr = address_from_pubkey(p.pk); p.key_file = tmpdir() + "/" + name + ".key";
    write_text(p.key_file, to_hex(p.sk) + "\n" + to_hex(p.pk) + "\n");
    return p;
}

int main() {
    const std::string exe = q(TAZ_NODE_BINARY);
    std::error_code ec;
    Person alice = person("alice"), bob = person("bob");
    const std::string chain = tmpdir() + "/rwa-chain.json";
    fs::remove(chain, ec);
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + to_hex(alice.addr) + ":1000000 --init-genesis " + to_hex(bob.addr) + ":1000000") == 0);
    Chain fresh; std::string err;
    CHECK(load_chain_json(chain, fresh, &err));
    const std::string pin = to_hex(fresh.genesis_commitment());
    const std::string base = exe + " --chain " + q(chain) + " --expect-genesis " + pin;
    auto as = [&](const Person& p) { return base + " --key-file " + q(p.key_file); };
    auto load = [&](Chain& c) { std::string e; const bool ok = load_chain_json(chain, c, &e); CHECK(ok); return ok; };
    const std::string doc1 = hexs(1, 32), doc2 = hexs(40, 32);

    // 1. a registration: a block is made, the asset is in the registry, with the right owner, kind, document hash and height
    CHECK(run(as(alice) + " --register-asset 2:" + doc1) == 0);
    const Hash32 id0 = asset_id(CHAIN_ID, alice.addr, 0);
    {
        Chain c; if (load(c)) {
            CHECK(c.height() == 1 && c.state().asset_count() == 1);
            const AssetRecord* r = c.state().find_asset(id0);
            CHECK(r && r->owner == alice.addr && r->asset_kind == 2 && to_hex(r->doc_hash) == doc1 && r->created_at == 1);
            CHECK(c.state().nonce(alice.addr) == 1 && c.state().balance(alice.addr) == 1000000 - FEE_TRANSFER_V1);
            CHECK(c.blocks().back().transactions.size() == 1 && c.blocks().back().transactions[0].kind == TxKind::RegisterAsset);
        }
    }
    // the command prints the id of the asset it made, so the user can use it with --transfer-asset
    {
        const std::string out = tmpdir() + "/out.txt";
        CHECK(run(as(alice) + " --register-asset 3:" + doc2 + " > " + q(out)) == 0);
        const Hash32 id1 = asset_id(CHAIN_ID, alice.addr, 1);
        CHECK(slurp(out).find(to_hex(id1)) != std::string::npos);
        Chain c; if (load(c)) {
            CHECK(c.height() == 2 && c.state().asset_count() == 2 && id1 != id0);   // the same asset id is never made twice
            CHECK(c.state().find_asset(id1) && c.state().find_asset(id1)->asset_kind == 3);
        }
    }
    // the same arguments again make a NEW asset (a new nonce), never a duplicate id
    CHECK(run(as(alice) + " --register-asset 3:" + doc2) == 0);
    { Chain c; if (load(c)) CHECK(c.height() == 3 && c.state().asset_count() == 3); }

    // 2. a transfer: the owner gives asset 0 to bob
    CHECK(run(as(alice) + " --transfer-asset " + to_hex(id0) + ":" + to_hex(bob.addr)) == 0);
    {
        Chain c; if (load(c)) {
            CHECK(c.height() == 4 && c.state().asset_count() == 3);
            const AssetRecord* r = c.state().find_asset(id0);
            CHECK(r && r->owner == bob.addr && r->asset_kind == 2 && to_hex(r->doc_hash) == doc1 && r->created_at == 1);
            CHECK(c.blocks().back().transactions[0].kind == TxKind::TransferAsset);
            CHECK(c.state().nonce(alice.addr) == 4);
        }
    }
    // the chain refuses (exit 5) and the file is not touched: alice no longer owns it, an unknown asset, a wrong signer
    {
        const std::string before = slurp(chain);
        CHECK(run(as(alice) + " --transfer-asset " + to_hex(id0) + ":" + to_hex(alice.addr)) == 5);       // not the owner any more
        CHECK(run(as(alice) + " --transfer-asset " + hexs(200, 32) + ":" + to_hex(bob.addr)) == 5);        // no such asset
        CHECK(run(as(bob) + " --transfer-asset " + to_hex(asset_id(CHAIN_ID, alice.addr, 1)) + ":" + to_hex(bob.addr)) == 5);   // bob does not own that one
        CHECK(slurp(chain) == before);
    }
    // bob, the new owner, passes it back
    CHECK(run(as(bob) + " --transfer-asset " + to_hex(id0) + ":" + to_hex(alice.addr)) == 0);
    { Chain c; if (load(c)) CHECK(c.height() == 5 && c.state().find_asset(id0) && c.state().find_asset(id0)->owner == alice.addr); }

    // 3. wrong usage is a usage error (exit 2) and writes nothing
    {
        const std::string before = slurp(chain);
        const std::string good_doc = hexs(7, 32);
        for (const std::string& bad : std::vector<std::string>{"0:" + good_doc, "5:" + good_doc, "9:" + good_doc, "x:" + good_doc, ":" + good_doc, "1:", "1", "1:" + good_doc.substr(1),
                                      "1:" + good_doc + "00", "1:" + std::string(63, 'a') + "z", "-1:" + good_doc, "01x:" + good_doc, " 1:" + good_doc})
            CHECK(run(as(alice) + " --register-asset " + q(bad)) == 2);
        const std::string id = to_hex(id0), to = to_hex(bob.addr);
        for (const std::string& bad : std::vector<std::string>{id, id + ":", ":" + to, id.substr(1) + ":" + to, id + "00:" + to, id + ":" + to.substr(1), id + ":" + to + "00",
                                      std::string(64, 'z') + ":" + to, id + ":" + std::string(40, 'g'), "nonsense"})
            CHECK(run(as(alice) + " --transfer-asset " + q(bad)) == 2);
        // no key: a transaction must be signed by someone
        CHECK(run(base + " --register-asset 1:" + good_doc) == 2);
        CHECK(run(base + " --transfer-asset " + id + ":" + to) == 2);
        // no pin
        CHECK(run(exe + " --chain " + q(chain) + " --key-file " + q(alice.key_file) + " --register-asset 1:" + good_doc) == 2);
        // given twice, or mixed with another action or a network mode: one thing, then stop
        CHECK(run(as(alice) + " --register-asset 1:" + good_doc + " --register-asset 1:" + good_doc) == 2);
        CHECK(run(as(alice) + " --transfer-asset " + id + ":" + to + " --transfer-asset " + id + ":" + to) == 2);
        CHECK(run(as(alice) + " --register-asset 1:" + good_doc + " --transfer-asset " + id + ":" + to) == 2);
        CHECK(run(as(alice) + " --register-asset 1:" + good_doc + " --transfer " + to + ":5") == 2);
        CHECK(run(as(alice) + " --transfer-asset " + id + ":" + to + " --transfer " + to + ":5") == 2);
        CHECK(run(as(alice) + " --register-asset 1:" + good_doc + " --peer 9") == 2);
        CHECK(run(as(alice) + " --transfer-asset " + id + ":" + to + " --serve 9") == 2);
        CHECK(run(as(alice) + " --transfer-asset " + id + ":" + to + " --listen 9") == 2);
        CHECK(run(as(alice) + " --register-asset 1:" + good_doc + " --api 9") == 2);                 // the read-only API mode is not an action either
        CHECK(run(as(alice) + " --transfer-asset " + id + ":" + to + " --api 9") == 2);
        CHECK(run(exe + " --new-key " + q(tmpdir() + "/nk.key") + " --register-asset 1:" + good_doc) == 2);
        CHECK(!fs::exists(tmpdir() + "/nk.key"));
        CHECK(slurp(chain) == before);
    }
    // 4. every legal kind and an upper-case hash work; kind 4 is the largest
    CHECK(run(as(alice) + " --register-asset 1:" + hexs(9, 32)) == 0);
    CHECK(run(as(alice) + " --register-asset 4:" + std::string(64, 'A')) == 0);
    { Chain c; if (load(c)) {
        CHECK(c.height() == 7 && c.state().asset_count() == 5);
        const AssetRecord* r = c.state().find_asset(asset_id(CHAIN_ID, alice.addr, 5));   // alice has made 5 transactions before this one: 3 registrations, 1 transfer, 1 registration
        CHECK(r && r->asset_kind == 4 && to_hex(r->doc_hash) == std::string(64, 'a'));
    } }
    // 5. the usage text mentions both commands
    {
        const std::string out = tmpdir() + "/help.txt";
        CHECK(run(exe + " --help > " + q(out)) == 0);
        const std::string h = slurp(out);
        CHECK(h.find("--register-asset") != std::string::npos && h.find("--transfer-asset") != std::string::npos);
    }
    fs::remove_all(tmpdir(), ec);
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "node rwa tests: all passed\n";
    return EXIT_SUCCESS;
}
