// v9.0: the node binary, tested as a process.
//
// The library tests prove load_chain_at_startup() behaves; this proves the *binary* does — that it exits with the
// right code, that it can create a chain, that it refuses a wrong pin, a malformed pin, and a missing pin even
// when the chain file is real and valid, and that creating a genesis never overwrites an existing chain file.
// Exit codes: 0 ok, 2 usage, 3 refused to load, 4 refused to overwrite.
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

    fs::remove(chain, ec);
    if (failures == 0) { std::cout << "node binary tests: all passed\n"; return 0; }
    std::cerr << "node binary tests: " << failures << " failure(s)\n";
    return 1;
}
