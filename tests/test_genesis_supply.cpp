// F11: the total genesis supply must fit in a uint64, so that State::circulating() can never overflow.
// Found by the consensus audit (audit/AUDIT_CONSENSUS_PERSISTENCE.md, F11): a genesis of 2^64-1 + 2^64-1 was accepted
// and circulating() then threw std::overflow_error.
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)
static Address new_addr() { PrivKey sk; PubKey pk; if (!generate_keypair(sk, pk)) std::exit(2); return address_from_pubkey(pk); }
static bool circulating_ok(const Chain& c, Amount expected) {
    try { return c.state().circulating() == expected; } catch (const std::exception&) { return false; }
}

int main() {
    const Amount max = std::numeric_limits<Amount>::max();
    const Address a = new_addr(), b = new_addr(), p = new_addr();
    { Chain c; CHECK(!c.init_genesis(a, max, b, max)); }                      // overflows the supply
    { Chain c; CHECK(!c.init_genesis(a, max, b, 1)); }                        // one over
    { Chain c; CHECK(c.init_genesis(a, max, b, 0)); CHECK(circulating_ok(c, max)); }          // exactly the maximum
    { Chain c; CHECK(c.init_genesis(a, max / 2, b, max - max / 2)); CHECK(circulating_ok(c, max)); }
    { Chain c; CHECK(!c.init_genesis(a, max, b, max, {p})); }                 // the producer-committing overload too
    { Chain c; CHECK(c.init_genesis(a, 1000, b, 0, {p})); CHECK(circulating_ok(c, 1000)); }
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "genesis supply tests: all passed (6 cases)\n";
    return EXIT_SUCCESS;
}
