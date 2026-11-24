// F1: a node that is far behind (more than 1000 blocks) must be able to catch up.
// Found by the consensus audit (audit/AUDIT_CONSENSUS_PERSISTENCE.md, F1): the node refused any peer more than 1000
// blocks ahead, and one conversation moves at most 200 blocks, so a new node could never join a chain that had
// grown past 1000 blocks. These tests start the real taznode binary on every platform and FAIL on the unfixed code.
//
//   f1  a new (genesis-only) node pulls 1101 blocks from a server, in several rounds, exit code 0
//   f2  a node at height 1000 catches up to 2500 (8 rounds)
//   f3  the same in the other direction: the connecting node is ahead and the listening node is far behind
//   f4  an honest peer at the same height still needs exactly one conversation (no endless rounds)
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/net.hpp"
#include "taz/persist.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifndef TAZ_NODE_BINARY
#error "TAZ_NODE_BINARY must be defined (see CMakeLists.txt)"
#endif

using namespace taz;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; if (!generate_keypair(k.sk, k.pk)) std::exit(2); k.addr = address_from_pubkey(k.pk); return k; }
static std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }

// ---- node-process helpers ------------------------------------------------------------------------------------------
static std::uint16_t free_port() {
    Listener l; std::string e;
    if (!listen_loopback(0, l, &e)) std::exit(2);
    return l.port;                                                // the listener closes here; the node binds it a moment later
}
static std::string node() { return std::string("\"") + TAZ_NODE_BINARY + "\""; }

// Start a command in the background with its output in a file. POSIX shells use "&"; Windows has no such thing,
// so cmd's own "start /b" does the same job. Returns whether the launcher itself succeeded — the node is still
// starting at that point, which is why every caller waits for a line in the output file afterwards.
static bool run_in_background(const std::string& command, const std::string& out) {
#ifdef _WIN32
    const std::string full = "start /b \"\" cmd /c \"" + command + " > \"" + out + "\" 2>&1\"";
#else
    const std::string full = command + " > \"" + out + "\" 2>&1 &";
#endif
    return std::system(full.c_str()) == 0;
}

// Start a command in the foreground with its output in a file.
//
// On Windows the command must go through cmd /c explicitly, with the same quoting as run_in_background. Handing
// std::system a command that already starts with a quoted program path lets cmd strip the wrong quotes, and the
// call then waits for something that never finishes — which is exactly how the first Windows run hung.
static int run_waiting(const std::string& command, const std::string& out) {
#ifdef _WIN32
    const std::string full = "cmd /c \"" + command + " > \"" + out + "\" 2>&1\"";
#else
    const std::string full = command + " > \"" + out + "\" 2>&1";
#endif
    return std::system(full.c_str());
}
static std::string pin(const Chain& c) { return to_hex(c.genesis_commitment()); }
static bool wait_for(const fs::path& file, const std::string& text, int seconds) {
    for (int i = 0; i < seconds * 20; ++i) {
        if (slurp(file).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
static std::uint64_t height_of(const fs::path& file, const Chain& like) {
    Chain c; LoadOptions o; o.require_genesis_pin = true; o.expected_genesis_commitment = like.genesis_commitment();
    std::string err;
    if (!load_chain_json(file.string(), c, &err, o)) { std::cerr << "cannot load " << file << ": " << err << "\n"; return 9999999; }
    return c.height();
}
// Empty blocks are the cheapest legal blocks: the sync path is what is under test, not transactions.
static void grow(Chain& c, std::uint64_t to_height) {
    while (c.height() < to_height) {
        const ChainResult r = c.append_block({}, static_cast<std::int64_t>(c.height() + 1));
        if (!r.ok) { std::cerr << "setup failed: " << r.error << "\n"; std::exit(2); }
    }
}
static Chain make_chain(std::uint64_t height, const Key& a, const Key& b) {
    Chain c; if (!c.init_genesis(a.addr, 1000, b.addr, 0)) std::exit(2);
    grow(c, height); return c;
}

// `server_chain` is served for `rounds` conversations; `client_chain` connects once (`--peer`, no --every). A node
// that is behind must keep going by itself until it has caught up, so a single client invocation has to do it all.
// Returns the client's exit code; both chain files stay in `dir`.
static int catch_up(const fs::path& dir, const Chain& server_chain, const Chain& client_chain, int rounds) {
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(server_chain, (dir / "S.json").string(), &err));
    CHECK(save_chain_json(client_chain, (dir / "C.json").string(), &err));
    const std::uint16_t port = free_port();
    const std::string out = (dir / "s.out").string();
    const std::string s_cmd = node() + " --chain \"" + (dir / "S.json").string() + "\" --expect-genesis " + pin(server_chain) +
                              " --serve " + std::to_string(port) + " --max-peers " + std::to_string(rounds) + " --serve-timeout 120 --allow-anonymous";
    CHECK(run_in_background(s_cmd, out));
    CHECK(wait_for(out, "listening", 10));
    const std::string c_cmd = node() + " --chain \"" + (dir / "C.json").string() + "\" --expect-genesis " + pin(client_chain) + " --peer " + std::to_string(port) + " --allow-anonymous";
    const int rc = run_waiting(c_cmd, (dir / "c.out").string());
    const std::string done = "served " + std::to_string(rounds) + " of " + std::to_string(rounds);
    if (!wait_for(out, done, 30)) {
        std::cerr << "  the server never reported \"" << done << "\". Its output:\n" << slurp(out) << "\n";
        std::cerr << "  the client said (exit " << rc << "):\n" << slurp(dir / "c.out") << "\n";
    }
    return rc;
}

static void f1_new_node_pulls_1101_blocks() {
    Key a = new_key(), b = new_key();
    Chain full = make_chain(1101, a, b), fresh = make_chain(0, a, b);
    const fs::path dir = fs::temp_directory_path() / "taz-f1-a";
    const int rc = catch_up(dir, full, fresh, 6);             // 1101 blocks = 6 conversations of at most 200
    CHECK(rc == 0);
    CHECK(height_of(dir / "C.json", full) == 1101);
    CHECK(slurp(dir / "c.out").find("more than 1000 is refused") == std::string::npos);
}
static void f2_node_at_1000_catches_up_to_2500() {
    Key a = new_key(), b = new_key();
    Chain full = make_chain(2500, a, b);
    Chain behind = full; CHECK(behind.truncate_to(1000).ok);
    const fs::path dir = fs::temp_directory_path() / "taz-f1-b";
    const int rc = catch_up(dir, full, behind, 8);            // 1500 blocks = 8 conversations
    CHECK(rc == 0);
    CHECK(height_of(dir / "C.json", full) == 2500);
}
static void f3_far_behind_listener_receives_from_connecting_node() {
    Key a = new_key(), b = new_key();
    Chain full = make_chain(1101, a, b), fresh = make_chain(0, a, b);
    const fs::path dir = fs::temp_directory_path() / "taz-f1-c";
    const int rc = catch_up(dir, fresh, full, 6);             // the server is the one that is behind
    CHECK(rc == 0);
    CHECK(height_of(dir / "S.json", full) == 1101);
}
static void f4_equal_height_is_one_conversation() {
    Key a = new_key(), b = new_key();
    Chain same = make_chain(30, a, b);
    const fs::path dir = fs::temp_directory_path() / "taz-f1-d";
    const int rc = catch_up(dir, same, same, 1);
    CHECK(rc == 0);
    CHECK(height_of(dir / "C.json", same) == 30);
}

int main() {
    { std::string e; if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return 1; } }
    f1_new_node_pulls_1101_blocks();
    f2_node_at_1000_catches_up_to_2500();
    f3_far_behind_listener_receives_from_connecting_node();
    f4_equal_height_is_one_conversation();
    std::error_code ec;
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) for (const char* d : {"taz-f1-a", "taz-f1-b", "taz-f1-c", "taz-f1-d"}) fs::remove_all(fs::temp_directory_path() / d, ec);
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "far-sync tests: all passed (4 cases)\n";
    return EXIT_SUCCESS;
}
