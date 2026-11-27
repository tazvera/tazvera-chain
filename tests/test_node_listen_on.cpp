// taznode --listen-on <address>: the IPv4 address that --listen and --serve wait on (default 127.0.0.1).
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

static std::string tmpdir() { const std::string d = (fs::temp_directory_path() / "taz-node-listen-on-tests").string(); std::error_code ec; fs::create_directories(d, ec); return d; }
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
    PrivKey sk{}, sk2{}; PubKey pk{}, pk2{};
    if (!generate_keypair(sk, pk) || !generate_keypair(sk2, pk2)) return EXIT_FAILURE;
    const std::string key_file = dir + "/a.key"; write_text(key_file, to_hex(sk) + "\n" + to_hex(pk) + "\n");
    const std::string chain = dir + "/server.json", copy = dir + "/client.json";
    fs::remove(chain, ec); fs::remove(copy, ec);
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + to_hex(address_from_pubkey(pk)) + ":1000000 --init-genesis " + to_hex(address_from_pubkey(pk2)) + ":0") == 0);
    Chain fresh; std::string err; CHECK(load_chain_json(chain, fresh, &err));
    const std::string pin = to_hex(fresh.genesis_commitment());
    const std::string pristine = dir + "/pristine.json";
    fs::copy_file(chain, copy);   // the client starts from the same genesis and stays behind
    fs::copy_file(chain, pristine);
    const std::string base = exe + " --chain " + q(chain) + " --expect-genesis " + pin;
    // the server gets one block more than the client
    CHECK(run(base + " --key-file " + q(key_file) + " --transfer " + to_hex(address_from_pubkey(pk2)) + ":10") == 0);

    // 1. usage errors: exit 2 and nothing written
    {
        const std::string before = slurp(chain);
        const std::string port = std::to_string(free_port());
        for (const std::string bad : {"", "localhost", "1.2.3", "1.2.3.4.5", "256.0.0.1", "01.2.3.4", "::1", "1.2.3.4:80", "0.0.0.0/0", "127.1", " 127.0.0.1", "any"})
            CHECK(run(base + " --serve " + port + " --listen-on " + q(bad)) == 2);
        for (const std::string bad : {"", "localhost", "999.1.1.1"})
            CHECK(run(base + " --listen " + port + " --listen-on " + q(bad)) == 2);
        CHECK(run(base + " --listen-on 127.0.0.1") == 2);                                           // nothing to listen for
        CHECK(run(base + " --peer " + port + " --listen-on 127.0.0.1") == 2);                      // --peer connects, it does not listen
        CHECK(run(base + " --api " + port + " --listen-on 127.0.0.1") == 2);                       // the API stays on loopback
        CHECK(run(base + " --key-file " + q(key_file) + " --transfer " + to_hex(address_from_pubkey(pk2)) + ":1 --listen-on 127.0.0.1") == 2);
        CHECK(run(base + " --serve " + port + " --listen-on 127.0.0.1 --listen-on 127.0.0.1") == 2);   // twice is a mistake, not "last one wins"
        CHECK(run(exe + " --new-key " + q(dir + "/nk.key") + " --listen-on 127.0.0.1") == 2);
        CHECK(!fs::exists(dir + "/nk.key"));
        CHECK(slurp(chain) == before);
    }
    // 2. an address that is not one of this machine's: the bind fails (exit 3) and the message names it
    {
        const std::string log = dir + "/foreign.log";
        CHECK(run(base + " --serve " + std::to_string(free_port()) + " --listen-on 203.0.113.77 --max-peers 1 --serve-timeout 5 > " + q(log) + " 2>&1") == 3);
        CHECK(slurp(log).find("203.0.113.77") != std::string::npos);
    }
    // 3. 0.0.0.0 really serves a peer; the node says it is open beyond this machine
    {
        const std::uint16_t port = free_port(); const std::string log = dir + "/any.log";
        int server_rc = -1;
        std::thread server([&] { server_rc = run(base + " --serve " + std::to_string(port) + " --listen-on 0.0.0.0 --max-peers 1 --serve-timeout 30 > " + q(log) + " 2>&1"); });
        const bool ready = wait_for(log, "listening on 0.0.0.0:" + std::to_string(port));
        CHECK(ready);
        const int client_rc = ready ? run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer " + std::to_string(port) + "") : -1;
        server.join();
        CHECK(client_rc == 0 && server_rc == 0);
        const std::string out = slurp(log);
        CHECK(out.find("will serve 1 peer") != std::string::npos);
        CHECK(out.find("warning") != std::string::npos && out.find("0.0.0.0") != std::string::npos);
        Chain c; std::string e2; CHECK(load_chain_json(copy, c, &e2) && c.height() == 1);   // the client caught up through that connection
    }
    // 4. an explicit loopback address, and no flag at all, behave as before: the same line, no warning
    for (const std::string extra : {std::string(" --listen-on 127.0.0.1"), std::string()}) {
        const std::uint16_t port = free_port(); const std::string log = dir + "/loop.log";
        fs::remove(copy, ec); fs::copy_file(pristine, copy);
        int server_rc = -1;
        std::thread server([&] { server_rc = run(base + " --listen " + std::to_string(port) + extra + " --serve-timeout 30 > " + q(log) + " 2>&1"); });
        const bool ready = wait_for(log, "listening on 127.0.0.1:" + std::to_string(port));
        CHECK(ready);
        const int client_rc = ready ? run(exe + " --chain " + q(copy) + " --expect-genesis " + pin + " --peer " + std::to_string(port) + "") : -1;
        server.join();
        CHECK(client_rc == 0 && server_rc == 0);
        CHECK(slurp(log).find("warning") == std::string::npos);
        Chain c; std::string e4; CHECK(load_chain_json(copy, c, &e4) && c.height() == 1);
        fs::remove(log, ec);
    }
    // 5. the usage text names the flag
    {
        const std::string out = dir + "/help.txt";
        CHECK(run(exe + " --help > " + q(out)) == 0);
        CHECK(slurp(out).find("--listen-on") != std::string::npos);
    }
    fs::remove_all(dir, ec);
    net_cleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "node listen-on tests: all passed\n";
    return EXIT_SUCCESS;
}
