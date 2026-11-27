// F7: slot skipping. A producer who does not make its block no longer stops the chain: every full slot_seconds that
// passed since the parent block moves the turn on by one producer. The slot length is part of the genesis (so it is
// committed), a chain without it behaves exactly as before, and nothing here needs a new block field: the
// timestamp is already in the block hash that the producer signs.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/fork.hpp"
#include "taz/persist.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using namespace taz;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() {
    Key k;
    if (!generate_keypair(k.sk, k.pk)) { std::cerr << "keygen failed\n"; std::exit(2); }
    k.addr = address_from_pubkey(k.pk);
    return k;
}
static std::vector<Key> keys;   // p0..p6, made once
static std::vector<Address> addrs(std::size_t n) { std::vector<Address> v; for (std::size_t i = 0; i < n; ++i) v.push_back(keys[i].addr); return v; }

static constexpr std::int64_t T0 = 1000;   // the timestamp of block 1 in most tests

// A chain with n producers and slot length D, at height 1 (made by p0 at T0).
static Chain make_chain(std::size_t n, std::uint32_t D, bool with_block1 = true) {
    Chain c;
    if (!c.init_genesis(keys[10].addr, 1000, keys[11].addr, 0, addrs(n), D)) { std::cerr << "genesis refused\n"; std::exit(2); }
    if (with_block1 && !c.append_block_signed({}, T0, keys[0].sk, keys[0].pk).ok) { std::cerr << "block 1 refused\n"; std::exit(2); }
    return c;
}
// The signing message, as the chain defines it (domain, chain id, 0, u64be height, block hash).
static std::vector<std::uint8_t> signable(const Block& b) {
    std::vector<std::uint8_t> out;
    const std::string domain = "TAZVERA-BLOCK-SIG-V2";
    out.insert(out.end(), domain.begin(), domain.end());
    out.insert(out.end(), b.chain_id.begin(), b.chain_id.end());
    out.push_back(0);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((b.height >> (8 * i)) & 0xff));
    const Hash32 h = b.hash();
    out.insert(out.end(), h.begin(), h.end());
    return out;
}
// An empty block on top of `base` with this timestamp, signed by ANY key (right or wrong): lets a test offer the
// chain a block that only the slot rule can refuse. Uses an unsigned append on a copy to get the right state root.
static Block block_by(const Chain& base, std::int64_t ts, const Key& k) {
    Chain copy = base;
    if (!copy.append_block({}, ts).ok) { std::cerr << "cannot build the block\n"; std::exit(2); }
    Block b = copy.blocks().back();
    b.producer = k.addr; b.producer_pubkey = k.pk;
    if (!sign(k.sk, signable(b), b.producer_signature)) std::exit(2);
    return b;
}
static bool accepts(const Chain& base, std::int64_t ts, const Key& k) {
    Chain c = base;
    return c.accept_block(block_by(base, ts, k)).ok;
}
// Independent model of the rule, written differently from the implementation: index of the expected producer.
static std::size_t model_index(std::uint64_t next_height, std::int64_t parent_ts, std::int64_t ts, std::uint32_t D, std::size_t n) {
    if (next_height == 1) return 0;
    const std::uint64_t skips = static_cast<std::uint64_t>(ts - parent_ts) / D;
    return static_cast<std::size_t>(((next_height - 1) % n + skips % n) % n);
}

// slot_seconds == 0 is the chain of v9: the same commitment, the same round robin, whatever the timestamps
static void t01_legacy_unchanged() {
    const auto p = addrs(3);
    CHECK(genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p, 0) == genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p));
    Chain a, b;
    CHECK(a.init_genesis(keys[10].addr, 1000, keys[11].addr, 0, p));
    CHECK(b.init_genesis(keys[10].addr, 1000, keys[11].addr, 0, p, 0));
    CHECK(a.genesis_commitment() == b.genesis_commitment());
    CHECK(b.slot_seconds() == 0 && a.slot_seconds() == 0);
    CHECK(b.max_future_seconds() == 120);
    CHECK(b.append_block_signed({}, 10, keys[0].sk, keys[0].pk).ok);
    CHECK(b.expected_producer(11) == keys[1].addr);
    CHECK(b.expected_producer(11 + 1000000) == keys[1].addr);              // a huge gap changes nothing without slots
    CHECK(!accepts(b, 11 + 1000000, keys[2]));
    CHECK(accepts(b, 11 + 1000000, keys[1]));
}
// the slot length is committed, and out-of-range values are refused
static void t02_commitment_and_range() {
    const auto p = addrs(3);
    const Hash32 c0 = genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p, 0);
    const Hash32 c8 = genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p, 8);
    const Hash32 c9 = genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p, 9);
    CHECK(c0 != c8 && c8 != c9 && c0 != c9);
    CHECK(c8 == genesis_commitment_of(keys[10].addr, 1000, keys[11].addr, 0, p, 8));
    Chain c; CHECK(c.init_genesis(keys[10].addr, 1000, keys[11].addr, 0, p, 8));
    CHECK(c.genesis_commitment() == c8 && c.slot_seconds() == 8);
    CHECK(c.blocks()[0].previous_hash == c8);                                     // block 0 carries it
    for (std::uint32_t bad : {1u, 3u, 86401u, 4000000000u}) { Chain x; CHECK(!x.init_genesis(keys[10].addr, 1, keys[11].addr, 0, p, bad)); CHECK(x.blocks().empty()); }
    for (std::uint32_t good : {4u, 86400u}) { Chain x; CHECK(x.init_genesis(keys[10].addr, 1, keys[11].addr, 0, p, good)); }
    { Chain x; CHECK(!x.init_genesis(keys[10].addr, 1, keys[11].addr, 0, {}, 8)); }   // slots without producers mean nothing
}
// the schedule, with wrap-around (3 producers, 40 s slots, block 1 at T0 by p0)
static void t03_schedule() {
    const Chain c = make_chain(3, 40);
    CHECK(c.expected_producer(T0 + 1)  == keys[1].addr);
    CHECK(c.expected_producer(T0 + 39) == keys[1].addr);
    CHECK(c.expected_producer(T0 + 40) == keys[2].addr);    // one full slot passed: p1 lost its turn
    CHECK(c.expected_producer(T0 + 79) == keys[2].addr);
    CHECK(c.expected_producer(T0 + 80) == keys[0].addr);    // two slots: p0
    CHECK(c.expected_producer(T0 + 119) == keys[0].addr);
    CHECK(c.expected_producer(T0 + 120) == keys[1].addr);   // three slots: all the way round, p1 again
    // the first block has no real parent time (genesis is stamped 0): nothing can be skipped there
    const Chain g = make_chain(3, 40, false);
    CHECK(g.expected_producer(1) == keys[0].addr);
    CHECK(g.expected_producer(1000000000) == keys[0].addr);
    CHECK(!accepts(g, 1000000000, keys[1]));
}
// early skip refused, the right producer accepted, a producer who lost its turn cannot fill it late, and a refused block changes nothing
static void t04_accept_and_refuse() {
    const Chain c = make_chain(3, 40);
    CHECK(accepts(c, T0 + 10, keys[1]));
    CHECK(!accepts(c, T0 + 10, keys[2]));    // early skip
    CHECK(!accepts(c, T0 + 10, keys[0]));    // wrong producer
    CHECK(!accepts(c, T0 + 40, keys[1]));    // its slot is over
    CHECK(accepts(c, T0 + 40, keys[2]));
    CHECK(!accepts(c, T0 + 80, keys[2]));
    CHECK(accepts(c, T0 + 80, keys[0]));
    Chain d = c;
    const Hash32 root = d.state().state_root(); const Hash32 tip = d.blocks().back().hash();
    const auto r = d.accept_block(block_by(c, T0 + 10, keys[2]));
    CHECK(!r.ok && r.error.find("expected") != std::string::npos);
    CHECK(d.height() == 1 && d.state().state_root() == root && d.blocks().back().hash() == tip);
    // the live signer refuses too, and says it is not its turn
    Chain e = c;
    const auto s = e.append_block_signed({}, T0 + 10, keys[2].sk, keys[2].pk);
    CHECK(!s.ok && e.height() == 1);
    CHECK(e.append_block_signed({}, T0 + 40, keys[2].sk, keys[2].pk).ok);
    // not strictly after the parent: still refused whoever signs
    CHECK(!accepts(c, T0, keys[1]) && !accepts(c, T0 - 5, keys[1]));
}
// the exact boundary
static void t05_boundary() {
    const Chain c = make_chain(2, 40);
    CHECK(accepts(c, T0 + 1, keys[1]));
    CHECK(accepts(c, T0 + 39, keys[1]));
    CHECK(!accepts(c, T0 + 40, keys[1]));
    CHECK(accepts(c, T0 + 40, keys[0]));
    CHECK(accepts(c, T0 + 79, keys[0]));
    CHECK(!accepts(c, T0 + 80, keys[0]));
    CHECK(accepts(c, T0 + 80, keys[1]));
}
// after a skip the height-based turn continues: p2 made height 2 for p1, so p2 is also next at height 3, then p0
static void t06_after_a_skip() {
    Chain c = make_chain(3, 40);
    CHECK(c.append_block_signed({}, T0 + 40, keys[2].sk, keys[2].pk).ok);   // height 2, skipped p1
    CHECK(c.expected_producer(T0 + 41) == keys[2].addr);                    // height 3 is p2's own turn
    CHECK(c.append_block_signed({}, T0 + 41, keys[2].sk, keys[2].pk).ok);
    CHECK(c.expected_producer(T0 + 42) == keys[0].addr);                    // height 4
    CHECK(c.append_block_signed({}, T0 + 42, keys[0].sk, keys[0].pk).ok);
}
// extremes must not overflow or misbehave
static void t07_extremes() {
    const Chain c = make_chain(3, 40);
    const std::int64_t big = std::numeric_limits<std::int64_t>::max();
    CHECK(c.expected_producer(big) == keys[model_index(2, T0, big, 40, 3)].addr);
    CHECK(c.expected_producer(T0 + 1) == keys[1].addr);
    const Chain d = make_chain(7, 4);
    CHECK(d.expected_producer(big) == keys[model_index(2, T0, big, 4, 7)].addr);
    CHECK(d.expected_producer(T0 + 4) == keys[2].addr);
    Chain f = make_chain(3, 86400);
    CHECK(f.expected_producer(T0 + 86399) == keys[1].addr && f.expected_producer(T0 + 86400) == keys[2].addr);
}
// the clock: a live block may be at most a quarter slot ahead, so a producer cannot reach for a later slot early
static void t08_future_drift() {
    CHECK(make_chain(3, 40).max_future_seconds() == 10);
    CHECK(make_chain(3, 4).max_future_seconds() == 1);
    CHECK(make_chain(3, 600).max_future_seconds() == 120);
    CHECK(make_chain(3, 86400).max_future_seconds() == 120);
    const Chain c = make_chain(3, 40);
    const Block b = block_by(c, T0 + 40, keys[2]);   // p2's slot opens at T0 + 40
    { Chain d = c; CHECK(!d.accept_block(b, T0 + 20 + c.max_future_seconds()).ok); CHECK(d.height() == 1); }   // our clock says T0+20
    { Chain d = c; CHECK(!d.accept_block(b, T0 + 29 + c.max_future_seconds()).ok); }                          // T0+29: still early
    { Chain d = c; CHECK(d.accept_block(b, T0 + 30 + c.max_future_seconds()).ok); }                           // T0+30: within the allowance
    { Chain d = c; CHECK(d.accept_block(b).ok); }                                                              // replay: no clock, the rule is on committed data
}
// two blocks for one parent: both valid on their own, never both in a chain, and every node picks the same one
static void t09_competing_tips() {
    const Chain base = make_chain(3, 40);
    Chain a = base, b = base;
    const Block ba = block_by(base, T0 + 39, keys[1]);    // p1 at the end of its own slot
    const Block bb = block_by(base, T0 + 40, keys[2]);    // p2 at the start of its slot
    CHECK(a.accept_block(ba).ok && b.accept_block(bb).ok);
    CHECK(!a.accept_block(bb).ok && !b.accept_block(ba).ok);   // a chain never takes both
    CHECK(a.height() == b.height() && a.blocks().back().hash() != b.blocks().back().hash());
    const ForkDecision ab = choose_chain(a, b), ba_ = choose_chain(b, a);
    const bool a_wins = ba.hash() < bb.hash();
    // both sides reach the same verdict: exactly one of the two chains is kept, whichever is asked
    CHECK(ab.choice == (a_wins ? ForkChoice::KeepMine : ForkChoice::TakeTheirs));
    CHECK(ba_.choice == (a_wins ? ForkChoice::TakeTheirs : ForkChoice::KeepMine));
    // and the loser can really switch: truncate to the common parent and take the winner's block
    Chain loser = a_wins ? b : a;
    const Block& winner_block = a_wins ? ba : bb;
    CHECK(loser.truncate_to(1).ok && loser.accept_block(winner_block).ok);
    CHECK(loser.blocks().back().hash() == winner_block.hash());
}
// save and load: the slot length survives, is authenticated, and cannot be added or removed in the file
static void t10_persist() {
    Chain c = make_chain(3, 40);
    CHECK(c.append_block_signed({}, T0 + 40, keys[2].sk, keys[2].pk).ok);
    const std::string text = to_json(c);
    CHECK(text.find("\"genesis_slot_seconds\": 40") != std::string::npos);
    Chain back; std::string err;
    CHECK(load_chain_json_text(text, back, &err));
    CHECK(back.slot_seconds() == 40 && back.height() == 2);
    CHECK(back.genesis_commitment() == c.genesis_commitment() && back.blocks().back().hash() == c.blocks().back().hash());
    CHECK(back.expected_producer(T0 + 41) == c.expected_producer(T0 + 41));
    // the key removed: the commitment no longer matches
    std::string stripped = text;
    const std::size_t at = stripped.find("  \"genesis_slot_seconds\": 40,\n");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) stripped.erase(at, std::string("  \"genesis_slot_seconds\": 40,\n").size());
    Chain x; CHECK(!load_chain_json_text(stripped, x, &err));
    // another value: refused (commitment), and nonsense values are refused outright
    for (const char* v : {"41", "3", "0", "86401", "-40", "\"40\"", "40.0"}) {
        std::string t = text; const std::string key = "\"genesis_slot_seconds\": 40";
        t.replace(t.find(key), key.size(), std::string("\"genesis_slot_seconds\": ") + v);
        Chain y; CHECK(!load_chain_json_text(t, y, &err));
    }
    // a chain without slots writes no such key and loads as before
    Chain legacy; CHECK(legacy.init_genesis(keys[10].addr, 1000, keys[11].addr, 0, addrs(3)));
    CHECK(legacy.append_block_signed({}, T0, keys[0].sk, keys[0].pk).ok);
    const std::string lt = to_json(legacy);
    CHECK(lt.find("genesis_slot_seconds") == std::string::npos);
    Chain lb; CHECK(load_chain_json_text(lt, lb, &err) && lb.slot_seconds() == 0 && lb.height() == 1);
    // the key without producers is refused
    Chain none; CHECK(none.init_genesis(keys[10].addr, 1000, keys[11].addr, 0));
    std::string nt = to_json(none);
    nt.replace(nt.find("  \"blocks\":"), 0, "  \"genesis_slot_seconds\": 40,\n");
    Chain z; CHECK(!load_chain_json_text(nt, z, &err));
}
// a rebuild from genesis (this is what a fork switch does) keeps the slot rule
static void t11_truncate_keeps_slots() {
    Chain c = make_chain(3, 40);
    CHECK(c.append_block_signed({}, T0 + 40, keys[2].sk, keys[2].pk).ok);
    CHECK(c.append_block_signed({}, T0 + 41, keys[2].sk, keys[2].pk).ok);
    CHECK(c.truncate_to(1).ok);
    CHECK(c.slot_seconds() == 40 && c.height() == 1);
    CHECK(c.expected_producer(T0 + 40) == keys[2].addr && c.expected_producer(T0 + 10) == keys[1].addr);
}
// property: for random producer counts, slot lengths and gaps, exactly the modelled producer is accepted and nobody else
static void t12_property() {
    std::uint64_t x = 0x9e3779b97f4a7c15ULL;
    auto next = [&x]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    for (int round = 0; round < 40; ++round) {
        const std::size_t n = 1 + next() % 7;
        const std::uint32_t D = 4 + static_cast<std::uint32_t>(next() % 97);
        Chain c = make_chain(n, D);
        std::int64_t tip = T0;
        for (int step = 0; step < 4; ++step) {
            const std::int64_t gap = 1 + static_cast<std::int64_t>(next() % (static_cast<std::uint64_t>(D) * 9));
            const std::int64_t ts = tip + gap;
            const std::size_t want = model_index(c.height() + 1, tip, ts, D, n);
            CHECK(c.expected_producer(ts) == keys[want].addr);
            for (std::size_t k = 0; k < n; ++k) CHECK(accepts(c, ts, keys[k]) == (k == want));
            CHECK(c.append_block_signed({}, ts, keys[want].sk, keys[want].pk).ok);
            tip = ts;
        }
    }
}

int main() {
    for (int i = 0; i < 12; ++i) keys.push_back(new_key());
    t01_legacy_unchanged(); t02_commitment_and_range(); t03_schedule(); t04_accept_and_refuse(); t05_boundary();
    t06_after_a_skip(); t07_extremes(); t08_future_drift(); t09_competing_tips(); t10_persist();
    t11_truncate_keeps_slots(); t12_property();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "producer slot tests: all passed (12 cases)\n";
    return EXIT_SUCCESS;
}
