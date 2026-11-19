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
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: taznode --chain <file> --expect-genesis <64 hex>\n"
    "\n"
    "  --chain <file>            the chain file to load (created by a release, not by this node)\n"
    "  --expect-genesis <hex>    the genesis commitment you trust, taken from outside the file\n"
    "  --help                    this text\n"
    "\n"
    "A node that cannot name its chain does not start: without --expect-genesis this exits non-zero.\n";

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

}  // namespace

int main(int argc, char** argv) {
    std::string chain_file;
    std::string expect_hex;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { std::cout << kUsage; return 0; }
        if (a == "--chain" && i + 1 < argc) { chain_file = argv[++i]; continue; }
        if (a == "--expect-genesis" && i + 1 < argc) { expect_hex = argv[++i]; continue; }
        return usage_error("unknown argument \"" + a + "\"");
    }
    if (chain_file.empty()) return usage_error("--chain is required");
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
