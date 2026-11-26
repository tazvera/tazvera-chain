// v9.0: the node binary, tested as a process.
//
// The library tests prove load_chain_at_startup() behaves; this proves the *binary* does — that it exits with the
// right code, that it can create a chain, that it refuses a wrong pin, a malformed pin, and a missing pin even
// when the chain file is real and valid, and that creating a genesis never overwrites an existing chain file.
// It also produces a real block: a key signs a transfer, the node saves it, and a restart finds it at the same
// height with the same state root.
// Exit codes: 0 ok, 2 usage, 3 refused to load / refused peer / refused exclusive creation, 5 block refused.
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "taz/crypto.hpp"
#include "taz/persist.hpp"
#include "taz/net.hpp"

#ifndef TAZ_NODE_BINARY
#error "TAZ_NODE_BINARY must be defined (see CMakeLists.txt)"
#endif
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

static std::string tmpdir() {
    const std::string dir = (fs::temp_directory_path() / "taz-node-tests").string();
    std::error_code ec; fs::create_directories(dir, ec);
    return dir;
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

static void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string q(const std::string& s) { return "\"" + s + "\""; }

// std::system() returns a wait status on POSIX and the exit code on Windows; normalise it. On Windows the whole
// command gets one extra pair of quotes, otherwise cmd.exe strips the quotes around the program path and the
// arguments arrive mangled.
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

int main() {
    const std::string exe = std::string("\"") + TAZ_NODE_BINARY + "\"";
    const std::string chain = tmpdir() + "/node-chain.json";
    const std::string a0 = std::string(40, '1'), a1 = std::string(40, '2');
    std::error_code ec; fs::remove(chain, ec);

    // 1. create a chain: no pin needed. The commitment is read from the file this test just made, not from
    // stdout, so no shell redirection is involved.
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + a0 + ":1000 --init-genesis " + a1 + ":0") == 0);
    CHECK(fs::exists(chain));
    taz::Chain made; std::string err;
    const bool read_back = taz::load_chain_json(chain, made, &err);
    CHECK(read_back);
    if (!read_back) { std::cerr << "  " << err << "\n"; return 1; }
    const std::string pin = taz::to_hex(made.genesis_commitment());
    CHECK(pin.size() == 64);
    CHECK(made.height() == 0);

    // 1b. creating again over that file is refused, and the file it would have replaced is untouched
    const std::string before = slurp(chain);
    CHECK(!before.empty());
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + a0 + ":500 --init-genesis " + a1 + ":500") == 3);
    CHECK(slurp(chain) == before);                       // byte for byte, not "roughly the same"


    // 1c. Two independent taznode processes race to create one path. Exactly one may win.
    {
        const std::string race_chain = tmpdir() + "/node-create-race.json";
        const std::string log1 = tmpdir() + "/node-create-race-1.log";
        const std::string log2 = tmpdir() + "/node-create-race-2.log";
        fs::remove(race_chain, ec); fs::remove(log1, ec); fs::remove(log2, ec);
        const std::string args = " --chain " + q(race_chain) + " --init-genesis " + a0 + ":1000 --init-genesis " + a1 + ":0";
        std::atomic<int> ready{0}; std::atomic<bool> go{false};
        int rc1 = -1, rc2 = -1;
        std::thread p1([&] { ++ready; while (!go.load()) std::this_thread::yield(); rc1 = run(exe + args + " > " + q(log1) + " 2>&1"); });
        std::thread p2([&] { ++ready; while (!go.load()) std::this_thread::yield(); rc2 = run(exe + args + " > " + q(log2) + " 2>&1"); });
        while (ready.load() != 2) std::this_thread::yield();
        go.store(true); p1.join(); p2.join();
        CHECK((rc1 == 0 && rc2 != 0) || (rc2 == 0 && rc1 != 0));
        const std::string loser_log = rc1 == 0 ? slurp(log2) : slurp(log1);
        CHECK(loser_log.find("the chain file already exists; refusing to overwrite it (creation is exclusive)") != std::string::npos);
        fs::remove(race_chain, ec); fs::remove(race_chain + ".lock", ec); fs::remove(log1, ec); fs::remove(log2, ec);
    }

    // 2. the right pin opens it. Without --api the node must not open an HTTP listener.
    std::string api_probe_error;
    CHECK(taz::net_startup(&api_probe_error));
    taz::Listener unused_api_port;
    CHECK(taz::listen_loopback(0, unused_api_port, &api_probe_error));
    const std::uint16_t no_api_port = unused_api_port.port;
    unused_api_port.socket.close();
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + pin) == 0);
    taz::Socket api_probe;
    CHECK(!taz::connect_loopback(no_api_port, api_probe, &api_probe_error));
    taz::net_cleanup();

    // 3. a wrong (but well-formed) pin is refused
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + std::string(64, 'a')) == 3);

    // 4. a malformed pin is a usage error, not a load attempt
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis abc") == 2);
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + std::string(64, 'z')) == 2);

    // 4b. --new-key makes a key file, prints the address, and never replaces an existing one
    {
        const std::string k1 = tmpdir() + "/newkey1.txt";
        const std::string k2 = tmpdir() + "/newkey2.txt";
        fs::remove(k1, ec); fs::remove(k2, ec);
        CHECK(run(exe + " --new-key " + q(k1)) == 0);
        CHECK(run(exe + " --new-key " + q(k2)) == 0);
        const std::string a1 = slurp(k1), a2 = slurp(k2);
        // Two lines of 64 hex each — counted as lines, not as bytes. On Windows the text-mode writer turns every
        // newline into CRLF, so a byte count would be testing the line ending rather than the key.
        auto key_lines = [](const std::string& text) {
            std::vector<std::string> lines;
            std::string cur;
            for (char c : text) {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else if (c != '\r') cur.push_back(c);
            }
            if (!cur.empty()) lines.push_back(cur);
            return lines;
        };
        const auto l1 = key_lines(a1), l2 = key_lines(a2);
        CHECK(l1.size() == 2 && l2.size() == 2);
        for (const auto& line : l1) CHECK(line.size() == 64);
        for (const auto& line : l2) CHECK(line.size() == 64);
        CHECK(a1 != a2);                                    // a second key is a different key
        CHECK(run(exe + " --new-key " + q(k1)) != 0);       // the same path again is refused
        CHECK(slurp(k1) == a1);                             // and the file is untouched
    }

    // 5. a real, valid chain file and still no pin: the node must not start
    CHECK(run(exe + " --chain " + q(chain)) == 2);

    // 6. a missing file with a pin is refused, and it is not confused with a bad pin
    CHECK(run(exe + " --chain " + q(tmpdir() + "/does-not-exist.json") + " --expect-genesis " + pin) == 3);

    // 7. block production: a real key signs a transfer, the block is saved, and a restart sees it
    taz::PrivKey sk{};
    taz::PubKey pk{};
    if (!taz::generate_keypair(sk, pk)) { std::cerr << "keygen failed\n"; return 1; }
    taz::PrivKey sk2{};
    taz::PubKey pk2{};
    if (!taz::generate_keypair(sk2, pk2)) { std::cerr << "keygen failed\n"; return 1; }
    const std::string from_hex_addr = taz::to_hex(taz::address_from_pubkey(pk));
    const std::string to_hex_addr = taz::to_hex(taz::address_from_pubkey(pk2));

    const std::string prod_chain = tmpdir() + "/prod-chain.json";
    const std::string key_file = tmpdir() + "/producer.key";
    fs::remove(prod_chain, ec);
    write_text(key_file, taz::to_hex(sk) + "\n" + taz::to_hex(pk) + "\n");

    CHECK(run(exe + " --chain " + q(prod_chain) + " --init-genesis " + from_hex_addr + ":100000 --init-genesis " + to_hex_addr + ":0") == 0);
    taz::Chain fresh; std::string ferr;
    CHECK(taz::load_chain_json(prod_chain, fresh, &ferr));
    const std::string prod_pin = taz::to_hex(fresh.genesis_commitment());

    // a transfer needs a key; without one it is a usage error, not a silent no-op
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --transfer " + to_hex_addr + ":10") == 2);

    // produce one block
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --key-file " + q(key_file) + " --transfer " + to_hex_addr + ":10") == 0);

    taz::Chain after; std::string aerr;
    const bool reloaded = taz::load_chain_json(prod_chain, after, &aerr);
    CHECK(reloaded);
    if (reloaded) {
        CHECK(after.height() == 1);
        CHECK(after.state().balance(taz::address_from_pubkey(pk2)) == 10);
        CHECK(after.state().nonce(taz::address_from_pubkey(pk)) == 1);
        CHECK(after.state().balance(taz::address_from_pubkey(pk)) == 100000 - 10 - taz::FEE_TRANSFER_V1);
    }

    // two blocks in quick succession: the same second must not be refused for a clock reason
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --key-file " + q(key_file) + " --transfer " + to_hex_addr + ":5") == 0);
    taz::Chain twice; std::string terr;
    const bool twice_ok = taz::load_chain_json(prod_chain, twice, &terr);
    CHECK(twice_ok);
    if (twice_ok) {
        CHECK(twice.height() == 2);
        CHECK(twice.state().nonce(taz::address_from_pubkey(pk)) == 2);
        CHECK(twice.state().balance(taz::address_from_pubkey(pk2)) == 15);
    }

    // a key file whose public key does not belong to its seed is refused, and nothing is written
    const std::string bad_key = tmpdir() + "/bad.key";
    write_text(bad_key, taz::to_hex(sk) + "\n" + taz::to_hex(pk2) + "\n");
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --key-file " + q(bad_key) + " --transfer " + to_hex_addr + ":1") == 2);

    // a non-hex address is a usage error, not a garbage address
    CHECK(run(exe + " --chain " + q(tmpdir() + "/hex.json") + " --init-genesis " + std::string(40, 'z') + ":1 --init-genesis " + a1 + ":0") == 2);
    CHECK(!fs::exists(tmpdir() + "/hex.json"));

    // the same flag twice is an operator mistake, not "last one wins"
    CHECK(run(exe + " --chain " + q(prod_chain) + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin) == 2);

    // the handshake wiring: the arguments are checked, and a peer that is not there is an error, not a hang
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --listen 1 --peer 2") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer abc") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 0") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 1") == 3);   // nothing listening
    // strict peer authentication must not start unless this node also has an identity key
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin +
              " --require-identity --peer 1") == 2);

    // An identity file whose public key does not match its private seed must be refused before networking.
    const std::string bad_identity = tmpdir() + "/bad-identity.key";
    write_text(bad_identity, taz::to_hex(sk) + "\n" + taz::to_hex(pk2) + "\n");
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin +
              " --identity " + q(bad_identity) + " --require-identity --peer 1") == 2);
    fs::remove(bad_identity, ec);

    // Exercise the real taznode strict-authentication path between two separate processes.
    const std::string client_identity = tmpdir() + "/client-identity.key";
    write_text(client_identity, taz::to_hex(sk2) + "\n" + taz::to_hex(pk2) + "\n");
    std::string net_error;
    CHECK(taz::net_startup(&net_error));
    taz::Listener reserved_port;
    CHECK(taz::listen_loopback(0, reserved_port, &net_error));
    const std::uint16_t strict_port = reserved_port.port;
    reserved_port.socket.close();  // hand the OS-selected free port to the server process
    const std::string server_log = tmpdir() + "/strict-server.log";
    const std::string server_cmd = exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin +
        " --serve " + std::to_string(strict_port) + " --max-peers 1 --serve-timeout 8 --identity " +
        q(key_file) + " --require-identity > " + q(server_log) + " 2>&1";
    int server_rc = -1;
    std::thread server_thread([&] { server_rc = run(server_cmd); });
    bool server_ready = false;
    for (int attempt = 0; attempt < 50 && !server_ready; ++attempt) {
        server_ready = slurp(server_log).find("listening on 127.0.0.1:") != std::string::npos;
        if (!server_ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(server_ready);
    const int client_rc = server_ready
        ? run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin +
              " --peer " + std::to_string(strict_port) + " --identity " + q(client_identity) +
              " --require-identity")
        : -1;
    server_thread.join();
    CHECK(client_rc == 0);
    CHECK(server_rc == 0);

    // F12 allowlist regressions: CLI validation must happen before any socket is opened.
    const std::string allow_file = tmpdir() + "/allow-peers.txt";
    const std::string empty_allow_file = tmpdir() + "/empty-allow-peers.txt";
    const std::string bad_allow_file = tmpdir() + "/bad-allow-peers.txt";
    write_text(allow_file, taz::to_hex(taz::address_from_pubkey(pk2)) + "\n");
    write_text(empty_allow_file, "");
    write_text(bad_allow_file, std::string(39, 'a') + "\n");
    const std::string base_args = " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin;
    CHECK(run(exe + base_args + " --allow-peer " + taz::to_hex(taz::address_from_pubkey(pk2)) + " --peer 1") == 2);
    CHECK(run(exe + base_args + " --allow-peer " + std::string(40, 'z') +
              " --identity " + q(key_file) + " --require-identity --peer 1") == 2);
    CHECK(run(exe + base_args + " --allow-peer-file " + q(bad_allow_file) +
              " --identity " + q(key_file) + " --require-identity --peer 1") == 2);
    CHECK(run(exe + base_args + " --allow-peer-file " + q(allow_file) +
              " --identity " + q(key_file) + " --require-identity --peer 1") == 3);

    // Listed identity is accepted by the server; the file form is also exercised here.
    CHECK(taz::net_startup(&net_error));
    taz::Listener allow_reserved;
    CHECK(taz::listen_loopback(0, allow_reserved, &net_error));
    const std::uint16_t allow_port = allow_reserved.port;
    allow_reserved.socket.close();
    const std::string allow_log = tmpdir() + "/allow-server.log";
    const std::string allow_cmd = exe + base_args + " --serve " + std::to_string(allow_port) +
        " --max-peers 1 --serve-timeout 8 --identity " + q(key_file) +
        " --require-identity --allow-peer " + taz::to_hex(taz::address_from_pubkey(pk)) +
        " --allow-peer " + taz::to_hex(taz::address_from_pubkey(pk2)) +
        " --allow-peer-file " + q(allow_file) + " > " + q(allow_log) + " 2>&1";
    int allow_server_rc = -1;
    std::thread allow_server_thread([&] { allow_server_rc = run(allow_cmd); });
    bool allow_ready = false;
    for (int attempt = 0; attempt < 50 && !allow_ready; ++attempt) {
        allow_ready = slurp(allow_log).find("listening on 127.0.0.1:") != std::string::npos;
        if (!allow_ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(allow_ready);
    const int allow_client_rc = allow_ready
        ? run(exe + base_args + " --peer " + std::to_string(allow_port) +
              " --identity " + q(client_identity) + " --require-identity")
        : -1;
    allow_server_thread.join();
    CHECK(allow_client_rc == 0);
    CHECK(allow_server_rc == 0);
    fs::remove(allow_log, ec);

    // A peer whose valid identity is not listed is refused with an explicit allowlist reason.
    CHECK(taz::listen_loopback(0, allow_reserved, &net_error));
    const std::uint16_t deny_port = allow_reserved.port;
    allow_reserved.socket.close();
    const std::string deny_log = tmpdir() + "/deny-server.log";
    const std::string deny_cmd = exe + base_args + " --serve " + std::to_string(deny_port) +
        " --max-peers 1 --serve-timeout 2 --identity " + q(key_file) +
        " --require-identity --allow-peer " + taz::to_hex(taz::address_from_pubkey(pk)) +
        " > " + q(deny_log) + " 2>&1";
    int deny_server_rc = -1;
    std::thread deny_server_thread([&] { deny_server_rc = run(deny_cmd); });
    bool deny_ready = false;
    for (int attempt = 0; attempt < 50 && !deny_ready; ++attempt) {
        deny_ready = slurp(deny_log).find("listening on 127.0.0.1:") != std::string::npos;
        if (!deny_ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(deny_ready);
    const int deny_client_rc = deny_ready
        ? run(exe + base_args + " --peer " + std::to_string(deny_port) +
              " --identity " + q(client_identity) + " --require-identity")
        : -1;
    deny_server_thread.join();
    CHECK(deny_client_rc == 3);
    CHECK(deny_server_rc == 3);
    CHECK(slurp(deny_log).find("not in the peer allowlist") != std::string::npos);
    fs::remove(deny_log, ec);

    // An empty allowlist is intentionally deny-all, not an accidental allow-all.
    CHECK(taz::listen_loopback(0, allow_reserved, &net_error));
    const std::uint16_t empty_port = allow_reserved.port;
    allow_reserved.socket.close();
    const std::string empty_log = tmpdir() + "/empty-allow-server.log";
    const std::string empty_cmd = exe + base_args + " --serve " + std::to_string(empty_port) +
        " --max-peers 1 --serve-timeout 2 --identity " + q(key_file) +
        " --require-identity --allow-peer-file " + q(empty_allow_file) + " > " + q(empty_log) + " 2>&1";
    int empty_server_rc = -1;
    std::thread empty_server_thread([&] { empty_server_rc = run(empty_cmd); });
    bool empty_ready = false;
    for (int attempt = 0; attempt < 50 && !empty_ready; ++attempt) {
        empty_ready = slurp(empty_log).find("listening on 127.0.0.1:") != std::string::npos;
        if (!empty_ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(empty_ready);
    const int empty_client_rc = empty_ready
        ? run(exe + base_args + " --peer " + std::to_string(empty_port) +
              " --identity " + q(client_identity) + " --require-identity")
        : -1;
    empty_server_thread.join();
    CHECK(empty_client_rc == 3);
    CHECK(empty_server_rc == 3);
    CHECK(slurp(empty_log).find("not in the peer allowlist") != std::string::npos);
    fs::remove(empty_log, ec);
    fs::remove(allow_file, ec); fs::remove(empty_allow_file, ec); fs::remove(bad_allow_file, ec);

    taz::net_cleanup();
    fs::remove(client_identity, ec);
    fs::remove(server_log, ec);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --listen 1 --serve 2") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --max-peers 3") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --serve 1 --max-peers 0") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --every 5") == 2);            // needs --peer
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 1 --every 0") == 2);   // 1..3600
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 1 --every abc") == 2);
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --retries 3") == 2);              // needs --every
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 1 --every 1 --retries abc") == 2);
    // a peer that is not there is survived once and then given up on, without hanging
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --peer 1 --every 1 --retries 1") == 3);
    // --transfer does one thing and stops; combining it with a network mode used to do neither, silently
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin + " --transfer 22" +
              std::string(38, '2') + ":1 --peer 1") == 2);

    // restart: the same chain, the same height, the same state root — the block was really written
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin) == 0);
    taz::Chain restarted; std::string rerr;
    CHECK(taz::load_chain_json(prod_chain, restarted, &rerr));
    CHECK(restarted.height() == 2);
    if (twice_ok) CHECK(restarted.state().state_root() == twice.state().state_root());

    fs::remove(prod_chain, ec); fs::remove(key_file, ec); fs::remove(bad_key, ec);
    fs::remove(chain, ec);
    if (failures == 0) { std::cout << "node binary tests: all passed\n"; return 0; }
    std::cerr << "node binary tests: " << failures << " failure(s)\n";
    return 1;
}
