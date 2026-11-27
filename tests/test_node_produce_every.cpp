// taznode --produce-every <seconds>: with --serve, the node makes a block every N seconds (empty blocks are fine).
//
// Process-level: the real binary is run. Rules under test:
//  - usage: only with --serve, 1..3600, once; nothing else may be combined with it; usage errors write nothing (exit 2)
//  - timestamps strictly increase, also when the clock and the tip are close; a tip beyond the clock is not extended
//  - a chain that declares producers needs a key file of a declared producer, and the blocks are signed with it
//  - a producing node does not leave after --max-peers (default 5) peers: it is a node, not a one-shot server
//  - peers can fetch the new blocks
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
#ifndef _WIN32
#include <sys/wait.h>
#endif
#ifndef TAZ_NODE_BINARY
#error "TAZ_NODE_BINARY must be defined (see CMakeLists.txt)"
#endif

namespace fs = std::filesystem;
using namespace taz;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static std::string tmpdir() { const std::string d = (fs::temp_directory_path() / "taz-node-produce-every-tests").string(); std::error_code ec; fs::create_directories(d, ec); return d; }
static std::string slurp(const std::string& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static void write_text(const std::string& p, const std::string& t) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o << t; }
static std::string q(const std::string& s) { return "\"" + s + "\""; }
static int run(const std::string& cmd) {
#ifdef _WIN32
    const std::string full = "\"" + cmd + "\"";
#else
    const std::string& full = cmd;
#endif
    const int rc = std::system(full.c_str());
    if (rc == -1) return -1;
#ifdef _WIN32
    return rc;
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}
static std::uint16_t free_port() {
    Listener l; std::string e; if (!listen_loopback(0, l, &e)) std::exit(2);
    const std::uint16_t p = l.port; l.socket.close(); return p;
}
static bool wait_for(const std::string& file, const std::string& text) {
    for (int i = 0; i < 100; ++i) { if (slurp(file).find(text) != std::string::npos) return true; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    return false;
}
static std::int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// timestamps strictly increase from genesis to tip
static bool strictly_increasing(const Chain& c) {
    for (std::size_t i = 1; i < c.blocks().size(); ++i)
        if (c.blocks()[i].timestamp <= c.blocks()[i - 1].timestamp) return false;
    return true;
}

int main() {
    std::string e; if (!net_startup(&e)) return EXIT_FAILURE;
    const std::string exe = q(TAZ_NODE_BINARY), dir = tmpdir();
    std::error_code ec;
    PrivKey sk{}, sk2{}; PubKey pk{}, pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) return EXIT_FAILURE;
    const std::string key_file = dir + "/a.key", other_key = dir + "/b.key";
    write_text(key_file, to_hex(sk) + "\n" + to_hex(pk) + "\n");
    write_text(other_key, to_hex(sk2) + "\n" + to_hex(pk2) + "\n");
    const std::string pristine = dir + "/pristine.json";
    fs::remove(pristine, ec);
    CHECK(run(exe + " --chain " + q(pristine) + " --init-genesis " + to_hex(address_from_pubkey(pk)) + ":1000000 --init-genesis " + to_hex(address_from_pubkey(pk2)) + ":0") == 0);
    Chain fresh; std::string err; CHECK(load_chain_json(pristine, fresh, &err));
    const std::string pin = to_hex(fresh.genesis_commitment());
    auto fresh_copy = [&](const std::string& name) { const std::string p = dir + "/" + name; fs::remove(p, ec); fs::copy_file(pristine, p); return p; };

    // 1. usage errors: exit 2 and nothing written
    {
        const std::string chain = fresh_copy("usage.json");
        const std::string before = slurp(chain);
        const std::string base = exe + " --chain " + q(chain) + " --expect-genesis " + pin;
        const std::string port = std::to_string(free_port());
        for (const std::string bad : {"0", "3601", "-1", "abc", "", "1.5", "99999999999999999999", " 5"})
            CHECK(run(base + " --serve " + port + " --produce-every " + q(bad) + " --serve-timeout 2") == 2);
        CHECK(run(base + " --produce-every 5") == 2);                                              // nothing to serve
        CHECK(run(base + " --listen " + port + " --produce-every 5") == 2);                        // --listen serves one peer and stops
        CHECK(run(base + " --peer " + port + " --produce-every 5") == 2);
        CHECK(run(base + " --api " + port + " --produce-every 5") == 2);
        CHECK(run(base + " --key-file " + q(key_file) + " --transfer " + to_hex(address_from_pubkey(pk2)) + ":1 --produce-every 5") == 2);
        CHECK(run(base + " --serve " + port + " --produce-every 5 --produce-every 5 --serve-timeout 2") == 2);   // twice is a mistake
        CHECK(run(exe + " --new-key " + q(dir + "/nk.key") + " --produce-every 5") == 2);
        CHECK(!fs::exists(dir + "/nk.key"));
        CHECK(slurp(chain) == before);
    }
    // 2. an unsigned chain: blocks appear, empty, strictly increasing; the node stops at --serve-timeout with exit 0
    {
        const std::string chain = fresh_copy("plain.json"), log = dir + "/plain.log";
        const std::uint16_t port = free_port();
        const int rc = run(exe + " --chain " + q(chain) + " --expect-genesis " + pin + " --serve " + std::to_string(port) + " --produce-every 1 --serve-timeout 5 > " + q(log) + " 2>&1");
        CHECK(rc == 0);
        Chain c; std::string e2; CHECK(load_chain_json(chain, c, &e2));
        CHECK(c.height() >= 2 && c.height() <= 6);   // one block per second for five seconds: not a burst
        CHECK(strictly_increasing(c));
        for (std::size_t i = 1; i < c.blocks().size(); ++i) CHECK(c.blocks()[i].transactions.empty());
        CHECK(slurp(log).find("produced block height=1") != std::string::npos);
        if (failures) std::cerr << "---- node log (plain) ----\n" << slurp(log) << "---- height " << c.height() << " ----\n";
    }
    // 2b. a long interval does not hold back --serve-timeout: the node stops when it was told to, not at the next block time
    {
        const std::string chain = fresh_copy("slow.json");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + pin + " --serve " + std::to_string(free_port()) + " --produce-every 60 --serve-timeout 2") == 0);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
        Chain c; std::string e2b; CHECK(load_chain_json(chain, c, &e2b) && c.height() == 0);   // no block was due yet
    }
    // 3. a peer fetches the new blocks while the node runs; a producing node keeps serving past --max-peers (default 5)
    {
        const std::string chain = fresh_copy("live.json"), log = dir + "/live.log";
        const std::uint16_t port = free_port();
        int server_rc = -1;
        std::thread server([&] { server_rc = run(exe + " --chain " + q(chain) + " --expect-genesis " + pin + " --serve " + std::to_string(port) + " --produce-every 1 --serve-timeout 12 > " + q(log) + " 2>&1"); });
        const bool ready = wait_for(log, "listening on 127.0.0.1:" + std::to_string(port));
        CHECK(ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        const std::string copy = fresh_copy("live-client.json");
        bool all = ready;
        for (int i = 0; i < 6 && all; ++i)   // six peers: one more than the old default quota
            all = run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer " + std::to_string(port)) == 0;
        CHECK(all);
        server.join();
        CHECK(server_rc == 0);
        Chain c; std::string e3; CHECK(load_chain_json(copy, c, &e3) && c.height() >= 1);
        CHECK(slurp(log).find("stopped waiting after 12s") != std::string::npos);
        if (failures) std::cerr << "---- node log (live) ----\n" << slurp(log) << "---- client height " << c.height() << " ----\n";
    }
    // 4. a chain that declares producers: no key, or a key that is not a producer, is a usage error; the right key signs
    {
        const std::string chain = dir + "/prod.json", log = dir + "/prod.log";
        std::string pin2;   // declaring producers changes the genesis commitment, so this chain has its own pin
        { Chain c; std::string e4; CHECK(load_chain_json(pristine, c, &e4)); CHECK(c.set_producers({address_from_pubkey(pk)}).ok); CHECK(save_chain_json(c, chain, &e4)); pin2 = to_hex(c.genesis_commitment()); }
        const std::string before = slurp(chain);
        const std::string base = exe + " --chain " + q(chain) + " --expect-genesis " + pin2 + " --serve " + std::to_string(free_port()) + " --produce-every 1 --serve-timeout 2";
        CHECK(run(base + " > " + q(log) + " 2>&1") == 2);                   // nobody to sign with, and it says so
        CHECK(slurp(log).find("needs --key-file") != std::string::npos);
        CHECK(run(base + " --key-file " + q(other_key)) == 2);              // a key that is not a declared producer
        CHECK(slurp(chain) == before);
        const int rc = run(exe + " --chain " + q(chain) + " --expect-genesis " + pin2 + " --serve " + std::to_string(free_port()) + " --produce-every 1 --serve-timeout 5 --key-file " + q(key_file) + " > " + q(log) + " 2>&1");
        CHECK(rc == 0);
        Chain c; std::string e5; CHECK(load_chain_json(chain, c, &e5));   // the loader checks every signature and the round robin
        CHECK(c.height() >= 2 && strictly_increasing(c));
        CHECK(c.producers().size() == 1);
        CHECK(slurp(log).find("signed by the producer") != std::string::npos);
    }
    // 5. the clock and the tip: a tip a little ahead is stepped past (+1 each block); one beyond the allowed slack is not extended, and the node lives on
    {
        const std::string chain = dir + "/ahead.json", log = dir + "/ahead.log";
        std::int64_t tip = 0;
        { Chain c; std::string e6; CHECK(load_chain_json(pristine, c, &e6)); tip = now_s() + 60; CHECK(c.append_block({}, tip, now_s() + 120).ok); CHECK(save_chain_json(c, chain, &e6)); }
        CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + pin + " --serve " + std::to_string(free_port()) + " --produce-every 1 --serve-timeout 4 > " + q(log) + " 2>&1") == 0);
        Chain c; std::string e7; CHECK(load_chain_json(chain, c, &e7));
        CHECK(c.height() >= 3 && strictly_increasing(c));
        CHECK(c.blocks()[2].timestamp == tip + 1);                          // stepped past the tip, not the clock

        // a tip beyond the allowed slack is not extended; the node says so, keeps running and exits normally
        const std::string far = dir + "/far.json", log2 = dir + "/far.log";
        { Chain c2; std::string e8; CHECK(load_chain_json(pristine, c2, &e8)); CHECK(c2.append_block({}, now_s() + 500).ok); CHECK(save_chain_json(c2, far, &e8)); }
        CHECK(run(exe + " --chain " + q(far) + " --expect-genesis " + pin + " --serve " + std::to_string(free_port()) + " --produce-every 1 --serve-timeout 3 > " + q(log2) + " 2>&1") == 0);
        Chain c3; std::string e9; CHECK(load_chain_json(far, c3, &e9));
        CHECK(c3.height() == 1);                                            // nothing was added on top of that tip
        CHECK(slurp(log2).find("beyond this clock") != std::string::npos);
    }
    // 6. the usage text names the flag
    {
        const std::string out = dir + "/help.txt";
        CHECK(run(exe + " --help > " + q(out)) == 0);
        CHECK(slurp(out).find("--produce-every") != std::string::npos);
    }
    fs::remove_all(dir, ec);
    net_cleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "node produce-every tests: all passed\n";
    return EXIT_SUCCESS;
}
