// Reproductions for the consensus / persistence audit. Throwaway harness: it only USES the library; it changes no
// production file and is not part of the build. Build:
//   g++ -std=c++20 -O2 -Iinclude audit/repro_audit.cpp <build>/libtazcore.a -lcrypto -lpthread -o repro_audit
// Run:  repro_audit <case> [scratch-dir]      cases: replay persist cost split grind overflow files
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/fork.hpp"
#include "taz/persist.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace taz;
using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{};
    Key() { if (!generate_keypair(sk, pk)) std::exit(2); addr = address_from_pubkey(pk); } };

static Transaction transfer(const Key& k, const Address& to, Amount amount, std::uint64_t nonce) {
    Transaction tx; tx.from = k.addr; tx.to = to; tx.amount = amount; tx.nonce = nonce; tx.pubkey = k.pk;
    if (!sign(k.sk, tx.signable_bytes(), tx.signature)) std::exit(2);
    return tx;
}
static Address addr_n(std::uint32_t n) { Address a{}; a[0] = 0xee; for (int i = 0; i < 4; ++i) a[19 - i] = static_cast<std::uint8_t>(n >> (8 * i)); return a; }

// R1: a transaction signed for one TAZVERA chain is valid on every other chain with the same chain ID.
static int replay() {
    Key alice; const Address bob = addr_n(1), carol = addr_n(2);
    Chain a, b;                                   // two DIFFERENT networks (different genesis), same address and nonce
    a.init_genesis(alice.addr, 1'000'000, bob, 0);
    b.init_genesis(alice.addr, 5'000'000, carol, 0);
    std::cout << "genesis commitments differ: " << (a.genesis_commitment() != b.genesis_commitment()) << "\n";
    const Transaction tx = transfer(alice, bob, 400'000, 0);   // Alice signs ONCE, intending chain A only
    const auto ra = a.append_block({tx}, 10);
    const auto rb = b.append_block({tx}, 10);      // the same bytes, replayed on chain B
    std::cout << "chain A accepts the tx: " << ra.ok << "\n";
    std::cout << "chain B accepts the SAME tx: " << rb.ok << (rb.ok ? "   <-- REPRODUCED" : "   (not reproduced: " + rb.error + ")") << "\n";
    std::cout << "chain B: bob received " << b.state().balance(bob) << " that alice never authorised there\n";
    return rb.ok ? 0 : 1;
}

// R2: save_chain_json writes files that load_chain_json refuses (size / block-count / node limits).
static int persist(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::create_directories(dir);
    const std::uint64_t n = std::strtoull(std::getenv("BLOCKS") ? std::getenv("BLOCKS") : "130000", nullptr, 10);
    Key a, b;
    Chain c; c.init_genesis(a.addr, 1000, b.addr, 0);
    for (std::uint64_t i = 1; i <= n; ++i) if (!c.append_block({}, static_cast<std::int64_t>(i)).ok) { std::cerr << "append failed\n"; return 2; }
    const std::string path = dir + "/big.json";
    std::string err;
    std::cout << "built " << c.height() << " empty blocks (legal, no transactions)\n";
    std::cout << "save_chain_json: " << (save_chain_json(c, path, &err) ? "OK" : "FAILED " + err) << "\n";
    std::cout << "file size: " << fs::file_size(path) / (1024.0 * 1024.0) << " MiB\n";
    Chain back; err.clear();
    const bool loaded = load_chain_json(path, back, &err);
    std::cout << "load_chain_json of the file this node just wrote: " << (loaded ? "OK (not reproduced)" : "REFUSED: " + err + "   <-- REPRODUCED") << "\n";
    fs::remove(path);
    return loaded ? 1 : 0;
}

// R3: cost of the equal-height fork path (truncate_to) and of one invalid-late-tx block, as chain size grows.
static int cost() {
    Key rich; const Address sink = addr_n(9);
    Chain c; c.init_genesis(rich.addr, 1ull << 60, sink, 0);
    std::uint64_t nonce = 0; std::int64_t ts = 1;
    const int blocks = 20, per = 2000;
    auto t0 = Clock::now();
    for (int bI = 0; bI < blocks; ++bI) {
        std::vector<Transaction> txs;
        for (int i = 0; i < per; ++i) txs.push_back(transfer(rich, addr_n(100 + static_cast<std::uint32_t>(nonce)), 1, nonce)), ++nonce;
        if (!c.append_block(txs, ts++).ok) { std::cerr << "append failed\n"; return 2; }
    }
    std::cout << "built " << blocks << " blocks x " << per << " txs = " << blocks * per << " txs in " << ms_since(t0) << " ms\n";
    Chain copy = c;
    t0 = Clock::now();
    const auto r = copy.truncate_to(c.height() - 1);
    std::cout << "truncate_to(height-1) [what taznode does for ANY peer that claims our height with a different tip]: "
              << ms_since(t0) << " ms, ok=" << r.ok << "\n";
    // one block with 9999 valid transactions and a bad last one (needs a chain anyone can extend: no producers)
    std::vector<Transaction> txs;
    for (int i = 0; i < 9999; ++i) txs.push_back(transfer(rich, addr_n(500000 + static_cast<std::uint32_t>(i)), 1, nonce + static_cast<std::uint64_t>(i)));
    txs.push_back(transfer(rich, addr_n(7), 1, nonce + 123456));   // wrong nonce: the block is invalid
    Block bad; State cand;
    Chain probe = c;
    // validate_block() builds on the tip; header fields must be right for the check to reach the transactions.
    bad.height = c.height() + 1; bad.previous_hash = c.blocks().back().hash(); bad.timestamp = ts; bad.transactions = txs;
    t0 = Clock::now();
    const auto v = probe.validate_block(bad, NO_TIMESTAMP_LIMIT);
    std::cout << "validate_block(10000 txs, last one invalid): " << ms_since(t0) << " ms -> \"" << v.error << "\"\n";
    return 0;
}

// R4: after a fork two blocks deep the shorter side can never take the longer side's blocks.
static int split() {
    Key a, b; Chain base; base.init_genesis(a.addr, 1000, b.addr, 0);
    Chain A = base, B = base;
    A.append_block({}, 10); A.append_block({}, 20);         // A: g, a1, a2
    B.append_block({}, 11);                                  // B: g, b1   (a different block at height 1)
    const auto r = B.accept_block(A.blocks()[2], NO_TIMESTAMP_LIMIT);
    std::cout << "heights: A=" << A.height() << " B=" << B.height() << "; tips share block 1? "
              << (A.blocks()[1].hash() == B.blocks()[1].hash()) << "\n";
    std::cout << "B.accept_block(A's block 2): " << (r.ok ? "accepted (not reproduced)" : "refused: " + r.error + "   <-- REPRODUCED") << "\n";
    const auto d = choose_chain(B, A);
    std::cout << "choose_chain(B, A) says: " << (d.choice == ForkChoice::TakeTheirs ? "TakeTheirs" : "KeepMine") << " (" << d.reason
              << ") but taznode's sync only ever sends A's blocks from height 2, which B cannot attach\n";
    return r.ok ? 1 : 0;
}

// R5: a scheduled producer can choose which of its own two blocks wins the equal-height tie-break.
static int grind() {
    Key producer, other; Chain base;
    base.init_genesis(other.addr, 1'000'000, addr_n(1), 0, {producer.addr});
    const Key merchant;
    const Transaction pay = transfer(other, merchant.addr, 500'000, 0);
    Chain c1 = base; c1.append_block_signed({pay}, 100, producer.sk, producer.pk);     // block the merchant sees
    int tries = 0; std::int64_t ts = 100;
    Chain c2 = base;
    for (;; ++tries) {                                                                    // the rival: no payment, lower hash
        c2 = base;
        c2.append_block_signed({}, ++ts, producer.sk, producer.pk);
        if (c2.blocks().back().hash() < c1.blocks().back().hash()) break;
    }
    const auto d = choose_chain(c1, c2);
    std::cout << "block 1 (pays merchant) hash " << to_hex(c1.blocks().back().hash()).substr(0, 16) << "...\n";
    std::cout << "rival block 1 (no payment), found after " << tries + 1 << " try/tries, hash " << to_hex(c2.blocks().back().hash()).substr(0, 16) << "...\n";
    std::cout << "a node holding block 1 meets a node holding the rival: " << (d.choice == ForkChoice::TakeTheirs ? "it TAKES THE RIVAL (payment erased)   <-- REPRODUCED" : "keeps its block (not reproduced)") << "\n";
    return d.choice == ForkChoice::TakeTheirs ? 0 : 1;
}

// R6: nothing bounds total supply, so the library's own supply accessor throws on a legal genesis.
static int overflow() {
    Key a, b; Chain c;
    c.init_genesis(a.addr, ~0ull, b.addr, ~0ull);
    try { (void)c.state().circulating(); std::cout << "circulating() ok (not reproduced)\n"; return 1; }
    catch (const std::exception& e) { std::cout << "genesis of 2^64-1 + 2^64-1 is accepted; circulating() throws: " << e.what() << "   <-- REPRODUCED (library API only; the node never calls it)\n"; return 0; }
}

// R7 helper: writes full.json (n empty blocks) and genesis.json (same genesis, no blocks) and prints the pin.
static int files(const std::string& dir, std::uint64_t n) {
    std::filesystem::create_directories(dir);
    Key a, b; Chain c; c.init_genesis(a.addr, 1000, b.addr, 0);
    std::string err;
    if (!save_chain_json(c, dir + "/genesis.json", &err)) { std::cerr << err << "\n"; return 2; }
    for (std::uint64_t i = 1; i <= n; ++i) c.append_block({}, static_cast<std::int64_t>(i));
    if (!save_chain_json(c, dir + "/full.json", &err)) { std::cerr << err << "\n"; return 2; }
    std::cout << to_hex(c.genesis_commitment()) << "\n";
    return 0;
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "replay") return replay();
    if (which == "persist") return persist(argc > 2 ? argv[2] : "/tmp/taz-audit");
    if (which == "cost") return cost();
    if (which == "split") return split();
    if (which == "grind") return grind();
    if (which == "overflow") return overflow();
    if (which == "files") return files(argc > 2 ? argv[2] : "/tmp/taz-audit", argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1101);
    std::cerr << "usage: repro_audit replay|persist|cost|split|grind|overflow [dir]\n";
    return 2;
}
