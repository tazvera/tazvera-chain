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
#include <vector>
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif
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


// ---- helpers for the cases below -------------------------------------------------------------------------------------
// Holds the lock the way a running node does, without being a node, so the test controls exactly when it is held.
// (Implementation detail on purpose: the lock file is "<chain file>.lock", an flock / a no-sharing CreateFile.)
struct RawLock {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    bool take(const std::string& path) { h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); return h != INVALID_HANDLE_VALUE; }
    ~RawLock() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
#else
    int fd = -1;
    bool take(const std::string& path) { fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600); return fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0; }
    ~RawLock() { if (fd >= 0) ::close(fd); }
#endif
};
static std::string in_dir(const fs::path& dir, const std::string& command) {
#ifdef _WIN32
    return "cd /d \"" + dir.string() + "\" && " + command;
#else
    return "cd \"" + dir.string() + "\" && " + command;
#endif
}
static std::string init_cmd(const std::string& chain_arg, const Key& a, const Key& b) {
    return node() + " --chain " + chain_arg + " --init-genesis " + to_hex(a.addr) + ":1000 --init-genesis " + to_hex(b.addr) + ":0";
}

// l4: the lock comes BEFORE genesis creation. While another process holds the lock on a path, --init-genesis for that
// path must be refused and must not create the file (it used to check and write first, with no lock at all).
static void l4_init_genesis_needs_the_lock() {
    Key a = new_key(), b = new_key();
    const fs::path dir = fs::temp_directory_path() / "taz-f10-init";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    const fs::path chain = dir / "new.json";
    RawLock held; CHECK(held.take(chain.string() + ".lock"));
    const int rc = run_waiting(init_cmd("\"" + chain.string() + "\"", a, b), (dir / "init.out").string());
    CHECK(rc == 3);
    CHECK(!fs::exists(chain));                                       // nothing was written while someone else held the lock
    CHECK(slurp(dir / "init.out").find("in use by another") != std::string::npos);
}

// l5: the same chain file under another spelling must hit the same lock. Each spelling below used to get its own lock
// file and so evade the lock: relative path, "./" and "../" detours, a symlinked directory, a symlink to the file.
static void l5_other_spellings_of_the_path_are_still_locked() {
    Key a = new_key(), b = new_key();
    const Chain c = make_chain(2, a, b);
    const fs::path dir = fs::temp_directory_path() / "taz-f10-spell";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir / "sub");
    save(c, dir / "one.json");
    const std::uint16_t port = free_port();
    const std::string a_out = (dir / "a.out").string();
    CHECK(run_in_background(base_cmd(dir / "one.json", c) + " --listen " + std::to_string(port) + " --serve-timeout 40" + anon(), a_out));
    CHECK(wait_for(a_out, "listening", 10));

    auto refused = [&](const std::string& label, const std::string& command) {
        const int rc = run_waiting(command, (dir / "sp.out").string());
        const bool ok = rc == 3 && slurp(dir / "sp.out").find("in use by another") != std::string::npos;
        if (!ok) std::cerr << "  spelling \"" << label << "\" got past the lock (exit " << rc << "):\n" << slurp(dir / "sp.out") << "\n";
        CHECK(ok);
    };
    const std::string pin_arg = " --expect-genesis " + pin(c);
    refused("relative", in_dir(dir, node() + " --chain one.json" + pin_arg));
    refused("./sub/../", node() + " --chain \"" + (dir / "sub" / ".." / "one.json").string() + "\"" + pin_arg);
    refused("./", node() + " --chain \"" + (dir / "." / "one.json").string() + "\"" + pin_arg);
#ifndef _WIN32
    fs::create_directory_symlink(dir, fs::temp_directory_path() / "taz-f10-spell-link", ec);
    CHECK(!ec);
    refused("symlinked directory", node() + " --chain \"" + (fs::temp_directory_path() / "taz-f10-spell-link" / "one.json").string() + "\"" + pin_arg);
    fs::create_symlink(dir / "one.json", dir / "alias.json", ec);
    CHECK(!ec);
    refused("symlink to the file", node() + " --chain \"" + (dir / "alias.json").string() + "\"" + pin_arg);
    fs::remove(fs::temp_directory_path() / "taz-f10-spell-link", ec);
#endif
}

// l6: many pairs of --init-genesis started at the same moment: for every pair exactly one may succeed, and the file
// that is left must load. (Without the lock both could pass the "does it exist" check.)
static void l6_simultaneous_init_genesis_has_one_winner() {
    Key a = new_key(), b = new_key();
    const fs::path dir = fs::temp_directory_path() / "taz-f10-race";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    int bad_pairs = 0;
    for (int round = 0; round < 12; ++round) {
        const fs::path chain = dir / ("c" + std::to_string(round) + ".json");
        const std::string cmd = init_cmd("\"" + chain.string() + "\"", a, b);
        int rc1 = -1, rc2 = -1;
        std::thread t1([&] { rc1 = run_waiting(cmd, (dir / ("r" + std::to_string(round) + "a.out")).string()); });
        std::thread t2([&] { rc2 = run_waiting(cmd, (dir / ("r" + std::to_string(round) + "b.out")).string()); });
        t1.join(); t2.join();
        const int winners = (rc1 == 0) + (rc2 == 0);
        Chain back; std::string err;
        const bool loads = load_chain_json(chain.string(), back, &err);
        if (winners != 1 || !loads) { ++bad_pairs; std::cerr << "  round " << round << ": exit codes " << rc1 << " and " << rc2 << ", file loads: " << loads << "\n"; }
    }
    CHECK(bad_pairs == 0);
}

int main() {
    { std::string e; if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return 1; } }
    l1_l2_l3();
    l4_init_genesis_needs_the_lock();
    l5_other_spellings_of_the_path_are_still_locked();
    l6_simultaneous_init_genesis_has_one_winner();
    std::error_code ec;
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) for (const char* d : {"taz-f10", "taz-f10-init", "taz-f10-spell", "taz-f10-race"}) fs::remove_all(fs::temp_directory_path() / d, ec);
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "chain lock tests: all passed (6 cases)\n";
    return EXIT_SUCCESS;
}
