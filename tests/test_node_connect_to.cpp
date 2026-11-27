// taznode --peer <address>:<port>: connect to a named IPv4 address; the old form --peer <port> means 127.0.0.1:<port>.
//
// Process-level: the real binary is run. Usage errors write nothing and exit 2; an address that is not on this machine
// is refused at bind time (exit 3); 0.0.0.0 really serves a peer; the old default is unchanged.
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

static std::string tmpdir() { const std::string d = (fs::temp_directory_path() / "taz-node-connect-to-tests").string(); std::error_code ec; fs::create_directories(d, ec); return d; }
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

int main() {
    std::string e; if (!net_startup(&e)) return EXIT_FAILURE;
    const std::string exe = q(TAZ_NODE_BINARY), dir = tmpdir();
    std::error_code ec;
    fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    PrivKey sk{}, sk2{}; PubKey pk{}, pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) return EXIT_FAILURE;
    const std::string key_file = dir + "/a.key"; write_text(key_file, to_hex(sk) + "\n" + to_hex(pk) + "\n");
    const std::string chain = dir + "/server.json", pristine = dir + "/pristine.json";
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + to_hex(address_from_pubkey(pk)) + ":1000000 --init-genesis " + to_hex(address_from_pubkey(pk2)) + ":0") == 0);
    Chain fresh; std::string err; CHECK(load_chain_json(chain, fresh, &err));
    const std::string pin = to_hex(fresh.genesis_commitment());
    fs::copy_file(chain, pristine);   // clients start from the same genesis and stay behind
    const std::string base = exe + " --chain " + q(chain) + " --expect-genesis " + pin;
    CHECK(run(base + " --key-file " + q(key_file) + " --transfer " + to_hex(address_from_pubkey(pk2)) + ":10") == 0);   // the server is one block ahead
    auto client_chain = [&](const std::string& name) { const std::string p = dir + "/" + name; fs::remove(p, ec); fs::copy_file(pristine, p); return p; };

    // 1. usage errors: exit 2 and nothing written
    {
        const std::string copy = client_chain("usage.json"); const std::string before = slurp(copy);
        const std::string cbase = exe + " --chain " + q(copy) + " --expect-genesis " + pin;
        for (const std::string bad : {"127.0.0.1:", ":7001", "localhost:7001", "1.2.3:7001", "256.0.0.1:7001", "01.2.3.4:7001", "127.0.0.1:0", "127.0.0.1:65536",
                                      "127.0.0.1:abc", "127.0.0.1:70000", "127.0.0.1:7001:1", "[::1]:7001", " 127.0.0.1:7001", "127.0.0.1 :7001", "127.0.0.1:-1", "abc", "", "7001x"})
            CHECK(run(cbase + " --peer " + q(bad)) == 2);
        CHECK(run(cbase + " --peer 127.0.0.1:7001 --listen-on 127.0.0.1") == 2);      // --peer connects, it does not listen
        CHECK(run(cbase + " --peer 127.0.0.1:7001 --serve 7002") == 2);               // one mode at a time
        CHECK(run(cbase + " --peer 127.0.0.1:7001 --peer 127.0.0.1:7002") == 2);               // twice is a mistake, not "last one wins"
        CHECK(slurp(copy) == before);
    }
    // 2. the new form and the old form both sync from a node on 127.0.0.1
    for (const std::string form : {std::string("127.0.0.1:"), std::string()}) {
        const std::uint16_t port = free_port(); const std::string log = dir + "/serve.log";
        const std::string copy = client_chain("client.json");
        int server_rc = -1;
        std::thread server([&] { server_rc = run(base + " --listen " + std::to_string(port) + " --serve-timeout 30 > " + q(log) + " 2>&1"); });
        const bool ready = wait_for(log, "listening on 127.0.0.1:" + std::to_string(port));
        CHECK(ready);
        const int rc = ready ? run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer " + form + std::to_string(port)) : -1;
        server.join();
        CHECK(rc == 0 && server_rc == 0);
        Chain c; std::string e2; CHECK(load_chain_json(copy, c, &e2) && c.height() == 1);
    }
    // 3. nobody there: a clear message with address and port, exit 3, and no waiting around
    {
        const std::uint16_t port = free_port(); const std::string log = dir + "/refused.log";
        const std::string copy = client_chain("refused.json");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer 127.0.0.1:" + std::to_string(port) + " > " + q(log) + " 2>&1") == 3);
        CHECK(slurp(log).find("127.0.0.1:" + std::to_string(port)) != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    }
    // 4. an address that never answers ends at the connect limit (10 s), not at the operating system's
    {
        const std::string log = dir + "/silent.log"; const std::string copy = client_chain("silent.json");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer 203.0.113.77:7001 > " + q(log) + " 2>&1") == 3);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
        CHECK(slurp(log).find("203.0.113.77:7001") != std::string::npos);
    }
    // 5. the usage text shows the new form
    {
        const std::string out = dir + "/help.txt";
        CHECK(run(exe + " --help > " + q(out)) == 0);
        CHECK(slurp(out).find("--peer <address>:<port>") != std::string::npos);
    }
    fs::remove_all(dir, ec);
    net_cleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "node connect-to tests: all passed\n";
    return EXIT_SUCCESS;
}
