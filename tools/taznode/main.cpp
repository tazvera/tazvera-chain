// taznode — the first, deliberately small, node binary.
//
// What it does today: reads its configuration, loads the chain through load_chain_at_startup() (which refuses
// to load without a trusted genesis commitment), and prints what it loaded so an operator can compare the
// genesis commitment with the release notes.
//
// What it does NOT do: create a genesis, produce blocks, talk to other nodes, or listen on a socket. Those are
// the next steps, and none of them is pretended here.
#include "taz/startup.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: taznode --chain <file> --expect-genesis <64 hex>\n"
    "       taznode --chain <file> --init-genesis <40 hex address>:<amount> --init-genesis <40 hex>:<amount>\n"
    "\n"
    "  --chain <file>            the chain file to load or create\n"
    "  --expect-genesis <hex>    the genesis commitment you trust, taken from outside the file\n"
    "  --init-genesis <a>:<n>    create a new chain with these two genesis accounts (use exactly twice)\n"
    "  --help                    this text\n"
    "\n"
    "Loading a node that cannot name its chain does not start: without --expect-genesis it exits non-zero.\n"
    "Creating a chain needs no pin, because there is nothing yet to trust; it prints the commitment it made.\n";

int usage_error(const std::string& why) {
    std::cerr << "taznode: " << why << "\n\n" << kUsage;
    return 2;
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

std::vector<std::uint8_t> from_hex(const std::string& s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((nib(s[i]) << 4) | nib(s[i + 1])));
    return out;
}

bool parse_address(const std::string& s, taz::Address& out) {
    if (s.size() != 40) return false;
    const std::vector<std::uint8_t> raw = from_hex(s);
    if (raw.size() != out.size()) return false;
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = raw[i];
    return true;
}

bool parse_amount(const std::string& s, taz::Amount& out) {
    if (s.empty() || s.size() > 19) return false;
    taz::Amount v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<taz::Amount>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string chain_file;
    std::string expect_hex;
    std::vector<std::string> init_specs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { std::cout << kUsage; return 0; }
        if (a == "--chain" && i + 1 < argc) { chain_file = argv[++i]; continue; }
        if (a == "--expect-genesis" && i + 1 < argc) { expect_hex = argv[++i]; continue; }
        if (a == "--init-genesis" && i + 1 < argc) { init_specs.push_back(argv[++i]); continue; }
        return usage_error("unknown argument \"" + a + "\"");
    }
    if (chain_file.empty()) return usage_error("--chain is required");

    // Creating a chain: no pin is needed, because there is nothing to trust yet. The commitment this prints is
    // what an operator hands to everyone else, and what this node will demand on every later start.
    if (!init_specs.empty()) {
        if (init_specs.size() != 2) return usage_error("--init-genesis must be given exactly twice");
        taz::Address a0{}, a1{};
        taz::Amount n0 = 0, n1 = 0;
        const std::size_t c0 = init_specs[0].find(':');
        const std::size_t c1 = init_specs[1].find(':');
        if (c0 == std::string::npos || c1 == std::string::npos) return usage_error("--init-genesis wants <address>:<amount>");
        if (!parse_address(init_specs[0].substr(0, c0), a0) || !parse_address(init_specs[1].substr(0, c1), a1))
            return usage_error("a genesis address must be 40 hex characters");
        if (!parse_amount(init_specs[0].substr(c0 + 1), n0) || !parse_amount(init_specs[1].substr(c1 + 1), n1))
            return usage_error("a genesis amount must be a decimal number");
        taz::Chain fresh;
        if (!fresh.init_genesis(a0, n0, a1, n1)) return usage_error("genesis could not be created (same address twice?)");
        std::string save_err;
        if (!taz::save_chain_json(fresh, chain_file, &save_err)) {
            std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
            return 3;
        }
        std::cout << "taznode: created chain=" << chain_file
                  << " height=" << fresh.height()
                  << " genesis=" << taz::to_hex(fresh.genesis_commitment()) << "\n";
        return 0;
    }
    if (expect_hex.empty()) return usage_error("--expect-genesis is required: a node must be able to name its chain");
    if (!is_hex64(expect_hex)) return usage_error("--expect-genesis must be 64 hex characters");

    const std::vector<std::uint8_t> raw = from_hex(expect_hex);
    taz::Hash32 pin{};
    for (std::size_t i = 0; i < pin.size(); ++i) pin[i] = raw[i];

    taz::StartupConfig cfg;
    cfg.chain_file = chain_file;
    cfg.trusted_genesis = pin;

    taz::Chain chain;
    std::string err;
    const taz::StartupResult r = taz::load_chain_at_startup(cfg, chain, &err);
    if (!r.ok) {
        std::cerr << "taznode: " << (err.empty() ? r.error : err) << "\n";
        return 3;                                   // refused: wrong chain, unreadable file, or no pin
    }

    // One block an operator can compare with the release notes without opening a file.
    std::cout << "taznode: loaded"
              << " chain_id=" << taz::CHAIN_ID
              << " protocol=" << taz::PROTOCOL_VERSION
              << " core=" << taz::CORE_VERSION
              << " height=" << chain.height()
              << " genesis=" << taz::to_hex(chain.genesis_commitment())
              << " state_root=" << taz::to_hex(chain.state().state_root())
              << "\n";
    return 0;
}
