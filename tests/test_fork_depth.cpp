// F4/F5: a peer whose tip is at our height but does not attach to our history.
// Audit findings F4 (CPU amplification) and F5 (a deeper fork is reported as success), see
// audit/AUDIT_CONSENSUS_PERSISTENCE.md. Real taznode processes, both sides run the same equal-height fork steps.
//
//   d1  a fork two blocks deep (the peers share only the genesis): neither node may rebuild its chain from genesis
//       for it, neither may change its file, and both must report the failure (exit code 3) instead of success
//   d2  a fork one block deep still resolves as before: exit 0 and both nodes end on the same tip
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


static Chain forked(const Chain& base, std::uint64_t first_ts, std::uint64_t blocks) {
    Chain c = base;
    for (std::uint64_t i = 0; i < blocks; ++i) {
        const ChainResult r = c.append_block({}, static_cast<std::int64_t>(first_ts + i));
        if (!r.ok) { std::cerr << "setup failed: " << r.error << "\n"; std::exit(2); }
    }
    return c;
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
// Runs the real node twice: A listens (one peer), B connects. Returns B's exit code; A's code goes to *a_rc via its log.
static int meet(const fs::path& dir, const Chain& a, const Chain& b, std::string* a_log, std::string* b_log) {
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(a, (dir / "A.json").string(), &err));
    CHECK(save_chain_json(b, (dir / "B.json").string(), &err));
    const std::uint16_t port = free_port();
    const std::string a_out = (dir / "a.out").string();
    CHECK(run_in_background(node() + " --chain \"" + (dir / "A.json").string() + "\" --expect-genesis " + pin(a) +
                            " --listen " + std::to_string(port) + " --serve-timeout 60" + anon(), a_out));
    CHECK(wait_for(a_out, "listening", 10));
    const int rc = run_waiting(node() + " --chain \"" + (dir / "B.json").string() + "\" --expect-genesis " + pin(b) +
                               " --peer " + std::to_string(port) + anon(), (dir / "b.out").string());
    std::this_thread::sleep_for(std::chrono::seconds(2));    // let A finish and write its output
    *a_log = slurp(a_out); *b_log = slurp(dir / "b.out");
    return rc;
}
static Hash32 tip_of(const fs::path& file, const Chain& like) {
    Chain c; LoadOptions o; o.require_genesis_pin = true; o.expected_genesis_commitment = like.genesis_commitment();
    std::string err;
    if (!load_chain_json(file.string(), c, &err, o)) { std::cerr << "cannot load " << file << ": " << err << "\n"; return Hash32{}; }
    return c.blocks().back().hash();
}

static void d1_two_deep_fork_is_reported_and_changes_nothing() {
    Key a = new_key(), b = new_key();
    const Chain g = make_chain(0, a, b);
    const Chain ca = forked(g, 1, 2), cb = forked(g, 101, 2);         // same height 2, different blocks all the way down
    const fs::path dir = fs::temp_directory_path() / "taz-f45-a";
    std::string alog, blog;
    const int rc = meet(dir, ca, cb, &alog, &blog);
    CHECK(rc == 3);                                                    // F5: it used to report success (0)
    CHECK(blog.find("does not attach") != std::string::npos);          // F4: refused by a cheap header check
    CHECK(blog.find("took the peer's chain") == std::string::npos);
    CHECK(tip_of(dir / "A.json", g) == ca.blocks().back().hash());    // nothing changed on disk
    CHECK(tip_of(dir / "B.json", g) == cb.blocks().back().hash());
    if (blog.find("does not attach") == std::string::npos) std::cerr << "B said:\n" << blog << "A said:\n" << alog << "\n";
}
static void d2_one_deep_fork_still_resolves() {
    Key a = new_key(), b = new_key();
    const Chain base = forked(make_chain(0, a, b), 1, 1);
    const Chain ca = forked(base, 2, 1), cb = forked(base, 102, 1);   // share block 1, differ at block 2
    const fs::path dir = fs::temp_directory_path() / "taz-f45-b";
    std::string alog, blog;
    const int rc = meet(dir, ca, cb, &alog, &blog);
    CHECK(rc == 0);
    CHECK(tip_of(dir / "A.json", base) == tip_of(dir / "B.json", base));   // both nodes ended on the same tip
    if (rc != 0) std::cerr << "B said:\n" << blog << "A said:\n" << alog << "\n";
}

int main() {
    { std::string e; if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return 1; } }
    d1_two_deep_fork_is_reported_and_changes_nothing();
    d2_one_deep_fork_still_resolves();
    std::error_code ec;
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) for (const char* d : {"taz-f45-a", "taz-f45-b"}) fs::remove_all(fs::temp_directory_path() / d, ec);
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "fork depth tests: all passed (2 cases)\n";
    return EXIT_SUCCESS;
}
