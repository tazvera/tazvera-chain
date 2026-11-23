// Adversarial tests against the real taznode binary (PR #68 hardening).
//
// A real taznode --serve process is the victim; this test is the attacker, speaking the wire protocol by hand on a
// loopback socket. One server process takes a series of connections, each doing exactly one wrong thing, and the
// log of the server says why each was refused. A final honest peer proves the server was not simply refusing
// everything: it must be served, which also ends the server (--max-peers counts only completed peers).
//
//   Strict mode (--require-identity):
//     s1  anonymous hello (no identity at all)                         -> refused: peer did not prove an identity
//     s2  signed hello, then an anonymous (all-zero) proof             -> refused: key does not match
//     s3  signed hello by A, proof by B (valid for B)                  -> refused: key does not match
//     s4  signed hello by A, proof by A over the wrong transcript      -> refused: proof invalid
//     s5  honest peer                                                  -> served
//   Compatibility mode (no --require-identity):
//     c1  hello by A, proof by B                                       -> refused: proof key != hello identity
//     c2  hello by A, anonymous proof                                  -> refused: proof key != hello identity
//     c3  hello by A, proof by A over the wrong transcript             -> refused: proof does not verify
//     c4  hello by A with a corrupted signature                        -> refused: hello signature
//     c5  unsigned hello, valid proof by B                             -> served (identity comes from the proof)
//     c6  unsigned hello, anonymous proof                              -> served (anonymous, as documented)
//     c7  hello by A, proof by A                                       -> served
#include "taz/auth.hpp"
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/hello.hpp"
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
#include <utility>
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

static std::string tmpdir() {
    const std::string dir = (fs::temp_directory_path() / "taz-node-adversarial").string();
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

static int count_of(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + part.size())) ++n;
    return n;
}

struct Key {
    PrivKey sk{};
    PubKey pk{};
    Key() { if (!generate_keypair(sk, pk)) std::exit(2); }
    std::string file_text() const { return to_hex(sk) + "\n" + to_hex(pk) + "\n"; }
};

enum class HelloKind { Anonymous, Signed, SignedThenCorrupted };
enum class ProofKind { Proper, Zero, WrongTranscript };

struct Attack {
    const char* name;
    HelloKind hello;
    const Key* hello_key;     // signs the hello (unless Anonymous)
    ProofKind proof;
    const Key* proof_key;     // signs the proof (unless Zero)
    bool expect_served;
};

// Plays one attack. Returns true if the server sent kMsgAccept, i.e. it considered us a peer.
static bool attack(std::uint16_t port, const Chain& chain, const Attack& a, std::string& note) {
    Socket sock;
    if (!connect_loopback(port, sock, &note)) return false;
    const auto deadline = [] { return std::chrono::steady_clock::now() + std::chrono::seconds(5); };

    Hello mine = hello_for(chain);
    if (a.hello != HelloKind::Anonymous) {
        if (!sign_hello(mine, a.hello_key->sk)) { note = "cannot sign hello"; return false; }
        if (a.hello == HelloKind::SignedThenCorrupted) mine.signature[0] ^= 0x01;
    }
    const std::string my_raw = encode_hello(mine);
    if (!send_message(sock, kMsgHello, my_raw, &note, deadline())) return false;

    std::uint8_t type = 0;
    std::string their_raw;
    if (!recv_message(sock, type, their_raw, &note, deadline()) || type != kMsgHello) return false;
    Hello theirs;
    if (!decode_hello(their_raw, theirs, &note)) return false;

    // The server's own proof comes next if it got that far; if it refused our hello it just closes. Either is fine.
    std::string ignored;
    std::string ignore_note;
    recv_message(sock, type, ignored, &ignore_note, deadline());

    AuthMessage proof;   // all zero = anonymous
    if (a.proof != ProofKind::Zero) {
        std::string transcript = auth_transcript(mine, my_raw, theirs, their_raw);
        if (a.proof == ProofKind::WrongTranscript) {
            Hello other = mine;
            other.best_height += 7;
            transcript = auth_transcript(other, encode_hello(other), theirs, their_raw);
        }
        if (!sign_auth(proof, a.proof_key->sk, transcript)) { note = "cannot sign proof"; return false; }
    }
    send_message(sock, kMsgAuth, encode_auth(proof), &ignore_note, deadline());   // may fail if the server already closed

    std::string payload;
    if (!recv_message(sock, type, payload, &ignore_note, deadline())) return false;
    return type == kMsgAccept;
}

static std::uint16_t free_port() {
    Listener l; std::string err;
    if (!listen_loopback(0, l, &err)) { std::cerr << "no free port: " << err << "\n"; std::exit(2); }
    const std::uint16_t p = l.port;
    l.socket.close();
    return p;
}

// Starts the server, runs the attacks in order, and checks what each one got and what the server logged.
static void scenario(const std::string& title, const std::string& exe, const std::string& chain_path,
                     const std::string& pin, const Chain& chain, const std::string& identity_path,
                     bool strict, const std::vector<Attack>& attacks, int expected_served,
                     const std::vector<std::pair<std::string, int>>& log_expectations) {
    std::cerr << "[" << title << "]\n";
    const std::uint16_t port = free_port();
    const std::string log = tmpdir() + "/" + title + ".log";
    std::error_code ec; fs::remove(log, ec);
    const std::string cmd = exe + " --chain " + q(chain_path) + " --expect-genesis " + pin +
        " --serve " + std::to_string(port) + " --max-peers " + std::to_string(expected_served) +
        " --serve-timeout 30 --identity " + q(identity_path) + (strict ? " --require-identity" : "") +
        " > " + q(log) + " 2>&1";
    int rc = -1;
    std::thread server([&] { rc = run(cmd); });
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        ready = slurp(log).find("listening on 127.0.0.1:") != std::string::npos;
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(ready);
    if (ready) {
        for (const Attack& a : attacks) {
            std::string note;
            const bool got_accept = attack(port, chain, a, note);
            CHECK(got_accept == a.expect_served);
            if (got_accept != a.expect_served)
                std::cerr << "  " << a.name << ": expected " << (a.expect_served ? "served" : "refused")
                          << ", got " << (got_accept ? "served" : "refused") << " " << note << "\n";
        }
    }
    server.join();
    const std::string text = slurp(log);
    CHECK(rc == 3);   // refusals happened, so the node reports it (a clean 0 would mean nothing was ever refused)
    CHECK(count_of(text, "served " + std::to_string(expected_served) + " of " + std::to_string(expected_served)) == 1);
    for (const auto& [part, n] : log_expectations) {
        const int got = count_of(text, part);
        CHECK(got == n);
        if (got != n) std::cerr << "  log: \"" << part << "\" expected " << n << " got " << got << "\n";
    }
    if (failures) std::cerr << "---- server log ----\n" << text << "--------------------\n";
    fs::remove(log, ec);
}

int main() {
    std::string err;
    if (!net_startup(&err)) { std::cerr << "net_startup: " << err << "\n"; return 2; }
    const std::string exe = std::string("\"") + TAZ_NODE_BINARY + "\"";
    const std::string chain_path = tmpdir() + "/adv-chain.json";
    std::error_code ec; fs::remove(chain_path, ec);
    CHECK(run(exe + " --chain " + q(chain_path) + " --init-genesis " + std::string(40, '1') + ":1000 --init-genesis " +
              std::string(40, '2') + ":0") == 0);
    Chain chain;
    CHECK(load_chain_json(chain_path, chain, &err));
    const std::string pin = to_hex(chain.genesis_commitment());

    Key server_key, A, B;
    const std::string server_identity = tmpdir() + "/adv-server.key";
    write_text(server_identity, server_key.file_text());

    // ---- strict ----
    scenario("strict", exe, chain_path, pin, chain, server_identity, true,
        {
            {"s1 anonymous hello",              HelloKind::Anonymous, nullptr, ProofKind::Zero,   nullptr, false},
            {"s2 signed hello, anonymous proof", HelloKind::Signed,    &A,      ProofKind::Zero,   nullptr, false},
            {"s3 proof by another key",         HelloKind::Signed,    &A,      ProofKind::Proper, &B,      false},
            {"s4 proof over wrong transcript",  HelloKind::Signed,    &A,      ProofKind::WrongTranscript, &A, false},
            {"s5 honest peer",                  HelloKind::Signed,    &A,      ProofKind::Proper, &A,      true},
        },
        1,
        {
            {"authenticated handshake refused: peer did not prove an identity", 1},
            {"authenticated handshake refused: the authentication key does not match the signed hello", 2},
            {"authenticated handshake refused: the peer's connection-bound identity proof is invalid", 1},
            {"refused one connection", 4},
            {"peer is anonymous", 0},
        });

    // ---- compatibility ----
    scenario("compat", exe, chain_path, pin, chain, server_identity, false,
        {
            {"c1 hello by A, proof by B",        HelloKind::Signed,              &A,      ProofKind::Proper, &B, false},
            {"c2 hello by A, anonymous proof",   HelloKind::Signed,              &A,      ProofKind::Zero,   nullptr, false},
            {"c3 wrong transcript",              HelloKind::Signed,              &A,      ProofKind::WrongTranscript, &A, false},
            {"c4 corrupted hello signature",     HelloKind::SignedThenCorrupted, &A,      ProofKind::Proper, &A, false},
            {"c5 unsigned hello, proof by B",    HelloKind::Anonymous,           nullptr, ProofKind::Proper, &B, true},
            {"c6 unsigned hello, no proof",      HelloKind::Anonymous,           nullptr, ProofKind::Zero,   nullptr, true},
            {"c7 hello by A, proof by A",        HelloKind::Signed,              &A,      ProofKind::Proper, &A, true},
        },
        3,
        {
            {"the connection proof key does not match the signed hello identity", 2},
            {"the peer's identity proof does not verify; refused", 1},
            {"the peer sent an identity signature that does not verify", 1},
            {"refused one connection", 4},
            {"peer is anonymous (no identity proof)", 1},
            {"peer identity=" + to_hex(address_from_pubkey(B.pk)) + " (proof verified on this connection)", 1},
            {"peer identity=" + to_hex(address_from_pubkey(A.pk)) + " (proof verified on this connection)", 1},
        });

    fs::remove(server_identity, ec);
    fs::remove(chain_path, ec);
    net_cleanup();
    if (failures == 0) { std::cout << "node adversarial tests: all passed\n"; return 0; }
    std::cerr << "node adversarial tests: " << failures << " failure(s)\n";
    return 1;
}
