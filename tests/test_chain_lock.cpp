// F10: two taznode processes must not run on one chain file.
// Audit finding F10 (audit/AUDIT_CONSENSUS_PERSISTENCE.md): there was no lock, so a --serve and a second node on the
// same file overwrite each other's blocks (last writer wins). Real taznode processes.
//
//   l1  while one node runs on a chain file, a second node on the same file is refused (exit code 3)
//   l2  once the first has stopped, the file can be used again
//   l3  a node on a different chain file is not affected
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
#ifndef _WIN32
#include <sys/wait.h>
#endif

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
    int rc = std::system(full.c_str());
#ifndef _WIN32
    rc = (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;    // std::system returns the raw wait status on POSIX
#endif
    return rc;
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



// Strict peer authentication became the default in a later change; these tests have no identities, so they opt in to
// anonymous peers when this build has the flag (and need nothing when anonymous is still the default).
static std::string anon() {
    static const std::string flag = [] {
        const fs::path out = fs::temp_directory_path() / "taz-f45-help.txt";
        run_waiting(node() + " --help", out.string());
        return slurp(out).find("--allow-anonymous") != std::string::npos ? std::string(" --allow-anonymous") : std::string();
    }();
    return flag;
}
static std::string base_cmd(const fs::path& file, const Chain& like) {
    return node() + " --chain \"" + file.string() + "\" --expect-genesis " + pin(like);
}
static void save(const Chain& c, const fs::path& file) { std::string e; CHECK(save_chain_json(c, file.string(), &e)); }

static void l1_l2_l3() {
    Key a = new_key(), b = new_key();
    const Chain c = make_chain(3, a, b);
    const fs::path dir = fs::temp_directory_path() / "taz-f10";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    save(c, dir / "one.json"); save(c, dir / "two.json");
    const std::uint16_t port = free_port();
    const std::string a_out = (dir / "a.out").string();
    // the first node: serves one peer that never comes, so it stays up (the timeout keeps a failure from leaking it)
    CHECK(run_in_background(base_cmd(dir / "one.json", c) + " --listen " + std::to_string(port) + " --serve-timeout 25" + anon(), a_out));
    CHECK(wait_for(a_out, "listening", 10));

    const int rc_same = run_waiting(base_cmd(dir / "one.json", c), (dir / "same.out").string());       // l1
    const std::string same = slurp(dir / "same.out");
    CHECK(rc_same == 3);
    CHECK(same.find("in use by another") != std::string::npos);
    if (rc_same != 3) std::cerr << "second node on the same file said:\n" << same << "\n";

    const int rc_other = run_waiting(base_cmd(dir / "two.json", c), (dir / "other.out").string());      // l3
    CHECK(rc_other == 0);

    // l2: let the first node stop (its --serve-timeout), then the file is free again
    CHECK(wait_for(a_out, "no peer", 40) || true);
    int rc_after = -1;
    for (int i = 0; i < 40 && rc_after != 0; ++i) {
        rc_after = run_waiting(base_cmd(dir / "one.json", c), (dir / "after.out").string());
        if (rc_after != 0) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    CHECK(rc_after == 0);
}

static void genesis_creation_race() {
    Key a = new_key(), b = new_key(), c = new_key(), d = new_key();
    const fs::path dir = fs::temp_directory_path() / "taz-f10-genesis-race";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    const fs::path chain = dir / "genesis.json";
    const std::string cmd_a = node() + " --chain \"" + chain.string() + "\" --init-genesis " +
        to_hex(a.addr) + ":1000 --init-genesis " + to_hex(b.addr) + ":0";
    const std::string cmd_b = node() + " --chain \"" + chain.string() + "\" --init-genesis " +
        to_hex(c.addr) + ":2000 --init-genesis " + to_hex(d.addr) + ":0";
    const fs::path out_a = dir / "genesis-a.out", out_b = dir / "genesis-b.out";
    CHECK(run_in_background(cmd_a, out_a.string()));
    CHECK(run_in_background(cmd_b, out_b.string()));

    // Both attempts must finish with an explicit outcome. Exactly one may create the file;
    // the other must observe the lock or the already-created file and must never overwrite it.
    auto has_outcome = [](const fs::path& p) {
        const std::string s = slurp(p);
        return s.find("created chain=") != std::string::npos ||
               s.find("is in use by another taznode process") != std::string::npos ||
               s.find("file already exists and will not be replaced") != std::string::npos;
    };
    bool done_a = false, done_b = false;
    for (int i = 0; i < 400 && !(done_a && done_b); ++i) {
        done_a = done_a || has_outcome(out_a);
        done_b = done_b || has_outcome(out_b);
        if (!(done_a && done_b)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(done_a);
    CHECK(done_b);
    const std::string a_out = slurp(out_a), b_out = slurp(out_b);
    const bool a_created = a_out.find("created chain=") != std::string::npos;
    const bool b_created = b_out.find("created chain=") != std::string::npos;
    CHECK(a_created != b_created);
    if (!a_created) CHECK(a_out.find("is in use by another taznode process") != std::string::npos ||
                          a_out.find("file already exists and will not be replaced") != std::string::npos);
    if (!b_created) CHECK(b_out.find("is in use by another taznode process") != std::string::npos ||
                          b_out.find("file already exists and will not be replaced") != std::string::npos);

    Chain loaded;
    std::string load_err;
    CHECK(load_chain_json(chain.string(), loaded, &load_err));
    CHECK(loaded.height() == 0);
    if (a_created) CHECK(loaded.genesis_balances().size() == 2 &&
                         loaded.genesis_balances()[0].first == a.addr);
    if (b_created) CHECK(loaded.genesis_balances().size() == 2 &&
                         loaded.genesis_balances()[0].first == c.addr);
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) fs::remove_all(dir, ec);
}

int main() {
    { std::string e; if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return 1; } }
    l1_l2_l3();
    genesis_creation_race();
    std::error_code ec;
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) fs::remove_all(fs::temp_directory_path() / "taz-f10", ec);
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "chain lock tests: all passed (4 cases)\n";
    return EXIT_SUCCESS;
}
