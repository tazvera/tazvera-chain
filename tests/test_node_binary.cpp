// v9.0: the node binary, tested as a process.
//
// The library tests prove load_chain_at_startup() behaves; this proves the *binary* does — that it exits with the
// right code, that it can create a chain, that it refuses a wrong pin, a malformed pin, and a missing pin even
// when the chain file is real and valid, and that creating a genesis never overwrites an existing chain file.
// It also produces a real block: a key signs a transfer, the node saves it, and a restart finds it at the same
// height with the same state root.
// Exit codes: 0 ok, 2 usage, 3 refused to load, 4 refused to overwrite, 5 block refused.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include "taz/crypto.hpp"
#include "taz/persist.hpp"

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
    CHECK(run(exe + " --chain " + q(chain) + " --init-genesis " + a0 + ":500 --init-genesis " + a1 + ":500") == 4);
    CHECK(slurp(chain) == before);                       // byte for byte, not "roughly the same"

    // 2. the right pin opens it
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + pin) == 0);

    // 3. a wrong (but well-formed) pin is refused
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + std::string(64, 'a')) == 3);

    // 4. a malformed pin is a usage error, not a load attempt
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis abc") == 2);
    CHECK(run(exe + " --chain " + q(chain) + " --expect-genesis " + std::string(64, 'z')) == 2);

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

    // restart: the same chain, the same height, the same state root — the block was really written
    CHECK(run(exe + " --chain " + q(prod_chain) + " --expect-genesis " + prod_pin) == 0);
    taz::Chain restarted; std::string rerr;
    CHECK(taz::load_chain_json(prod_chain, restarted, &rerr));
    CHECK(restarted.height() == 1);
    if (reloaded) CHECK(restarted.state().state_root() == after.state().state_root());

    fs::remove(prod_chain, ec); fs::remove(key_file, ec);
    fs::remove(chain, ec);
    if (failures == 0) { std::cout << "node binary tests: all passed\n"; return 0; }
    std::cerr << "node binary tests: " << failures << " failure(s)\n";
    return 1;
}
