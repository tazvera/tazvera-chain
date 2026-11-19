// v9.0: the node binary, tested as a process.
//
// The library tests prove load_chain_at_startup() behaves; this proves the *binary* does — that it exits with the
// right code, that it can create a chain, that it refuses a wrong pin, a malformed pin, and a missing pin even
// when the chain file is real and valid.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

// std::system() returns a wait status on POSIX and the exit code on Windows; normalise it.
static int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
#ifdef _WIN32
    return rc;
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// "genesis=<64 hex>" out of the node's one output line
static std::string genesis_of(const std::string& text) {
    const std::string key = "genesis=";
    const std::size_t at = text.find(key);
    if (at == std::string::npos || at + key.size() + 64 > text.size()) return std::string();
    return text.substr(at + key.size(), 64);
}

int main() {
    const std::string exe = std::string("\"") + TAZ_NODE_BINARY + "\"";
    const std::string chain = tmpdir() + "/node-chain.json";
    const std::string out = tmpdir() + "/node-out.txt";
    const std::string a0 = std::string(40, '1'), a1 = std::string(40, '2');
    std::error_code ec; fs::remove(chain, ec);

    // 1. create a chain: no pin needed, and it prints the commitment it made
    CHECK(run(exe + " --chain \"" + chain + "\" --init-genesis " + a0 + ":1000 --init-genesis " + a1 + ":0 > \"" + out + "\"") == 0);
    CHECK(fs::exists(chain));
    const std::string created = slurp(out);
    const std::string pin = genesis_of(created);
    CHECK(pin.size() == 64);
    if (pin.size() != 64) { std::cerr << "created output was: " << created << "\n"; return 1; }

    // 2. the right pin opens it
    CHECK(run(exe + " --chain \"" + chain + "\" --expect-genesis " + pin + " > \"" + out + "\"") == 0);
    CHECK(slurp(out).find("height=0") != std::string::npos);

    // 3. a wrong (but well-formed) pin is refused
    const std::string wrong(64, 'a');
    CHECK(run(exe + " --chain \"" + chain + "\" --expect-genesis " + wrong + " > \"" + out + "\"") == 3);

    // 4. a malformed pin is a usage error, not a load attempt
    CHECK(run(exe + " --chain \"" + chain + "\" --expect-genesis abc > \"" + out + "\"") == 2);
    CHECK(run(exe + " --chain \"" + chain + "\" --expect-genesis " + std::string(64, 'z') + " > \"" + out + "\"") == 2);

    // 5. a real, valid chain file and still no pin: the node must not start
    CHECK(run(exe + " --chain \"" + chain + "\" > \"" + out + "\"") == 2);

    // 6. a missing file with a pin is refused, and it is not confused with a bad pin
    CHECK(run(exe + " --chain \"" + tmpdir() + "/does-not-exist.json\" --expect-genesis " + pin + " > \"" + out + "\"") == 3);

    fs::remove(chain, ec); fs::remove(out, ec);
    if (failures == 0) { std::cout << "node binary tests: all passed\n"; return 0; }
    std::cerr << "node binary tests: " << failures << " failure(s)\n";
    return 1;
}
