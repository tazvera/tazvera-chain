// F2: the node must never write a chain file that it cannot read back.
// Found by the consensus audit (audit/AUDIT_CONSENSUS_PERSISTENCE.md, F2): load_chain_json() refuses a file with more
// than 2,000,000 JSON values (about 117,000 empty blocks), more than 200,000 blocks or more than 64 MiB, while
// save_chain_json() had no matching check - empty blocks cost nothing to produce, so a node could save itself into a
// file it could not restart from.
//
// The property under test, at several sizes around the limit:
//   if save_chain_json() says OK, the file loads again and is the same chain;
//   if it refuses, the previous file is untouched and still loads.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace taz;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static Chain grow_to(Chain c, std::uint64_t height) {
    while (c.height() < height) {
        const ChainResult r = c.append_block({}, static_cast<std::int64_t>(c.height() + 1));
        if (!r.ok) { std::cerr << "setup failed: " << r.error << "\n"; std::exit(2); }
    }
    return c;
}

// Returns whether the save was accepted. Checks the property either way.
static bool check_property(const fs::path& file, const Chain& base, const Chain& bigger, const char* label) {
    std::string err;
    if (!save_chain_json(base, file.string(), &err)) { std::cerr << label << ": cannot save the base chain: " << err << "\n"; ++failures; return false; }
    LoadOptions pin; pin.require_genesis_pin = true; pin.expected_genesis_commitment = base.genesis_commitment();
    Chain back;
    const bool saved = save_chain_json(bigger, file.string(), &err);
    const Chain& expected = saved ? bigger : base;
    const bool loaded = load_chain_json(file.string(), back, &err, pin);
    if (!loaded) std::cerr << label << ": saved=" << saved << " but the file does not load: " << err << "\n";
    CHECK(loaded);
    CHECK(loaded && back.height() == expected.height());
    CHECK(loaded && back.blocks().back().hash() == expected.blocks().back().hash());
    std::cout << "  " << label << ": height " << bigger.height() << " -> " << (saved ? "saved and reloaded" : "refused, old file kept") << "\n";
    return saved;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "taz-f2";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    PrivKey ska, skb; PubKey pka, pkb;
    if (!generate_keypair(ska, pka) || !generate_keypair(skb, pkb)) return 2;
    Chain genesis; if (!genesis.init_genesis(address_from_pubkey(pka), 1000, address_from_pubkey(pkb), 0)) return 2;

    Chain at = grow_to(genesis, 100000);                       // well under every limit
    CHECK(check_property(dir / "a.json", genesis, at, "100000 blocks"));
    Chain mid = grow_to(at, 116000);                           // just under the 2,000,000-value limit
    CHECK(check_property(dir / "b.json", genesis, mid, "116000 blocks"));
    Chain over = grow_to(mid, 120000);                         // over it: about 2,050,000 values
    CHECK(!check_property(dir / "c.json", mid, over, "120000 blocks"));

    // Exact boundaries with small limits: save succeeds exactly when load would, for every limit alone.
    // (Added with the fix, which is what introduced the `limits` argument.)
    for (int which = 0; which < 3; ++which) {
        const char* name = which == 0 ? "max_blocks" : which == 1 ? "max_file_bytes" : "max_nodes";
        bool saw_ok = false, saw_refused = false;
        for (std::uint64_t h = 5; h <= 40; ++h) {
            Chain c = grow_to(genesis, h);
            LoadOptions lim;
            // limit sits at the size of the 20-block chain, so some heights fit and some do not
            const Chain ref = grow_to(genesis, 20);
            std::string e; const fs::path f = dir / "lim.json";
            LoadOptions loose; save_chain_json(ref, f.string(), &e, loose);
            std::uint64_t bytes = fs::file_size(f);
            lim.max_blocks = which == 0 ? 21 : lim.max_blocks;
            lim.max_file_bytes = which == 1 ? bytes : lim.max_file_bytes;
            if (which == 2) {                                     // smallest node limit that still lets the 20-block chain save
                std::uint64_t n = 1;
                for (;; ++n) { LoadOptions t; t.max_nodes = n; if (save_chain_json(ref, f.string(), &e, t)) break; }
                lim.max_nodes = n;
            }
            const bool saved = save_chain_json(c, f.string(), &e, lim);
            Chain back; LoadOptions load = lim; load.require_genesis_pin = true; load.expected_genesis_commitment = genesis.genesis_commitment();
            std::string e2;
            const bool loadable = saved ? load_chain_json(f.string(), back, &e2, load) : true;
            if (saved && !loadable) std::cerr << name << " h=" << h << ": saved but not loadable: " << e2 << "\n";
            CHECK(loadable);
            (saved ? saw_ok : saw_refused) = true;
        }
        CHECK(saw_ok); CHECK(saw_refused);                    // the sweep really crossed the limit
        std::cout << "  " << name << ": every saved file loads; limit crossed inside the sweep\n";
    }
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) fs::remove_all(dir, ec);
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "persist limit tests: all passed (6 cases)\n";
    return EXIT_SUCCESS;
}
