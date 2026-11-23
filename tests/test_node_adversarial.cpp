// Adversarial tests against the real taznode binary (PR #68 hardening).
//
// The real taznode is the victim and this test is the attacker, speaking the wire protocol by hand on loopback.
//
//   PART 1 - taznode --serve (server mode). One server process takes a series of connections, each doing exactly
//   one wrong thing. A final honest peer proves the server was not simply refusing everything: it must be served,
//   which also ends the server (--max-peers counts only completed peers).
//
//   PART 2 - taznode --peer (client mode). The test holds the LISTENING socket for the whole scenario and plays the
//   server; taznode connects to it. Because the test owns the port from before taznode starts until after it exits,
//   there is no port to race for. The attacker always finishes by sending kMsgAccept: a client that wrongly
//   believed a bad proof would then exit 0, so exit 0 really means "accepted" and cannot be confused with a
//   time-out.
//
// What is asserted: protocol outcomes (did the peer accept us, the exit code, what the honest client proved about
// itself, how long a silent peer was waited for). Not asserted: the wording of error messages. The few log lines
// that are checked are the node's progress contract ("served n of n", "refused one connection") and the identity
// it reports for an authenticated peer.
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
static int cases = 0;   // every attack that actually ran; printed at the end so a silently skipped test shows up
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

// taznode cannot be handed an already-bound socket, so a --serve scenario has to give it a port NUMBER. The number
// comes from the kernel (bind to port 0) and the reservation is closed only immediately before taznode starts.
// If another process took the port in that instant, taznode could not bind, never prints "listening on <port>",
// and the scenario FAILS LOUDLY with the server's own log. It cannot attack the wrong process: the attacker only
// starts after seeing taznode report that exact port, and a foreign listener would not speak the protocol.
// There is deliberately no retry. The client-mode scenarios below have no such window at all, because the test
// owns the listening socket throughout.
static std::uint16_t reserve_port() {
    Listener l; std::string err;
    if (!listen_loopback(0, l, &err)) { std::cerr << "no free port: " << err << "\n"; std::exit(2); }
    const std::uint16_t p = l.port;
    l.socket.close();
    return p;
}

// Starts the server, runs the attacks in order, and checks what each one got and what the server reported.
static void scenario(const std::string& title, const std::string& exe, const std::string& chain_path,
                     const std::string& pin, const Chain& chain, const std::string& identity_path,
                     bool strict, const std::vector<Attack>& attacks, int expected_served, int expected_refused,
                     const std::vector<std::string>& identities_in_log) {
    std::cerr << "[server mode: " << title << "]\n";
    const std::uint16_t port = reserve_port();
    const std::string log = tmpdir() + "/" + title + ".log";
    std::error_code ec; fs::remove(log, ec);
    const std::string cmd = exe + " --chain " + q(chain_path) + " --expect-genesis " + pin +
        " --serve " + std::to_string(port) + " --max-peers " + std::to_string(expected_served) +
        " --serve-timeout 30 --identity " + q(identity_path) + (strict ? " --require-identity" : "") +
        " > " + q(log) + " 2>&1";
    int rc = -1;
    std::thread server([&] { rc = run(cmd); });
    const std::string ready_line = "listening on 127.0.0.1:" + std::to_string(port) + " ";
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        ready = slurp(log).find(ready_line) != std::string::npos;
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(ready);
    if (!ready) std::cerr << "  taznode did not report listening on port " << port << " (port taken, or it failed to start); its log:\n" << slurp(log) << "\n";
    if (ready) {
        for (const Attack& a : attacks) {
            ++cases;
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
    CHECK(count_of(text, "refused one connection") == expected_refused);
    for (const std::string& addr : identities_in_log) CHECK(text.find(addr) != std::string::npos);
    if (failures) std::cerr << "---- server log ----\n" << text << "--------------------\n";
    fs::remove(log, ec);
}

// ===================================================================================================
// PART 2: taznode --peer. The attacker is the SERVER; the test owns the listening socket throughout.
// ===================================================================================================

struct Recording {                // what an honest server said on one connection, for replay on another
    std::string hello_raw;
    AuthMessage proof;
    bool valid = false;
};

enum class Behaviour { Normal, Silent, SilentAfterHello, Dribble, ReflectHello, Replay };

struct Script {
    const char* name;
    Behaviour how;
    HelloKind hello;
    const Key* hello_key;
    ProofKind proof;
    const Key* proof_key;
    bool expect_accepted;            // the client exits 0
    Recording* record = nullptr;     // fill from this connection
    const Recording* replay = nullptr;
};

struct Observed {
    bool saw_client_hello = false;
    bool client_proved_identity = false;   // its hello signature AND its connection-bound proof verified here
    PubKey client_pub{};
};

static void drain(Socket& s) {         // read until the other side closes (or 25 s), saying nothing
    std::uint8_t type = 0;
    std::string payload, note;
    while (recv_message(s, type, payload, &note, std::chrono::steady_clock::now() + std::chrono::seconds(25))) {}
}

static void play_server(Socket& s, const Chain& chain, const Script& sc, Observed& obs) {
    const auto dl = [] { return std::chrono::steady_clock::now() + std::chrono::seconds(25); };
    std::string note;
    if (sc.how == Behaviour::Silent) { drain(s); return; }

    std::uint8_t type = 0;
    std::string client_raw;
    if (!recv_message(s, type, client_raw, &note, dl()) || type != kMsgHello) return;
    Hello client;
    if (!decode_hello(client_raw, client, &note)) return;
    obs.saw_client_hello = true;
    obs.client_pub = client.pubkey;
    if (sc.how == Behaviour::ReflectHello) {          // play the client's own hello back to it
        send_message(s, kMsgHello, client_raw, &note, dl());
        drain(s);
        return;
    }

    if (sc.how == Behaviour::Dribble) {
        // A frame header announcing 100 bytes, then one byte per second. Every byte arrives inside the 3 s idle
        // timeout, so only the absolute 10 s deadline can end this.
        std::string frame = std::string(3, '\0') + std::string(1, '\x64') + std::string(1, static_cast<char>(kMsgHello));
        for (int i = 0; i < 20; ++i) {
            if (!send_all(s, frame, &note)) return;
            frame = "x";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return;
    }

    Hello mine = hello_for(chain);
    std::string my_raw;
    if (sc.how == Behaviour::Replay) {
        my_raw = sc.replay->hello_raw;                // byte for byte what an earlier connection saw
        if (!decode_hello(my_raw, mine, &note)) return;
    } else {
        if (sc.hello != HelloKind::Anonymous) {
            if (!sign_hello(mine, sc.hello_key->sk)) return;
            if (sc.hello == HelloKind::SignedThenCorrupted) mine.signature[0] ^= 0x01;
        }
        my_raw = encode_hello(mine);
    }
    if (!send_message(s, kMsgHello, my_raw, &note, dl())) return;
    if (sc.how == Behaviour::SilentAfterHello) { drain(s); return; }

    const std::string transcript = auth_transcript(client, client_raw, mine, my_raw);

    // The client sends its proof before reading ours, if it accepted our hello at all.
    std::string payload;
    AuthMessage client_auth;
    if (recv_message(s, type, payload, &note, dl()) && type == kMsgAuth && decode_auth(payload, client_auth, &note))
        obs.client_proved_identity = hello_signature_ok(client) && client_auth.pubkey == client.pubkey &&
                                     auth_ok(client_auth, transcript);

    AuthMessage proof;                                // all zero = anonymous
    if (sc.how == Behaviour::Replay) {
        proof = sc.replay->proof;
    } else if (sc.proof != ProofKind::Zero) {
        std::string t = transcript;
        if (sc.proof == ProofKind::WrongTranscript) {
            Hello other = client;
            other.best_height += 7;
            t = auth_transcript(other, encode_hello(other), mine, my_raw);
        }
        if (!sign_auth(proof, sc.proof_key->sk, t)) return;
    }
    if (sc.record) { sc.record->hello_raw = my_raw; sc.record->proof = proof; sc.record->valid = true; }

    // Always confirm, like a hostile server would: if the client wrongly trusted the proof it would exit 0 here.
    send_message(s, kMsgAuth, encode_auth(proof), &note, dl());
    send_message(s, kMsgAccept, {}, &note, dl());
    drain(s);
}

struct ClientEnv {
    std::string exe, chain_path, pin, identity_path;
    const Chain* chain;
};

struct ClientResult { int rc = -1; double seconds = 0; Observed obs; };

static ClientResult run_client(const ClientEnv& env, bool strict, const Script& sc) {
    ++cases;
    ClientResult r;
    Listener listener; std::string err;
    if (!listen_loopback(0, listener, &err)) { std::cerr << "cannot listen: " << err << "\n"; std::exit(2); }
    // From here until this function returns, this listener owns the port: taznode is given a port that is already
    // bound and listening, so nothing else can take it.
    std::thread attacker([&] {
        Socket peer; std::string note;
        if (!accept_one(listener.socket, peer, &note, std::chrono::steady_clock::now() + std::chrono::seconds(30))) {
            std::cerr << "  " << sc.name << ": taznode never connected: " << note << "\n";
            return;
        }
        play_server(peer, *env.chain, sc, r.obs);
    });
    const std::string log = tmpdir() + "/client.log";
    const std::string cmd = env.exe + " --chain " + q(env.chain_path) + " --expect-genesis " + env.pin +
        " --peer " + std::to_string(listener.port) + " --identity " + q(env.identity_path) +
        (strict ? " --require-identity" : "") + " > " + q(log) + " 2>&1";
    const auto t0 = std::chrono::steady_clock::now();
    r.rc = run(cmd);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    attacker.join();
    const bool right = sc.expect_accepted ? r.rc == 0 : r.rc == 3;
    CHECK(right);
    if (!right) std::cerr << "  " << sc.name << ": expected " << (sc.expect_accepted ? "exit 0" : "exit 3")
                          << ", got " << r.rc << "; client log:\n" << slurp(log) << "\n";
    std::error_code ec; fs::remove(log, ec);
    return r;
}

static void client_scenarios(const ClientEnv& env, const Key& client_key, const Key& A, const Key& B) {
    std::cerr << "[client mode: strict]\n";
    Recording honest;

    // k0 control: an honest authenticated server. The client must be accepted AND must have proved its own key.
    {
        Script sc{"k0 honest server", Behaviour::Normal, HelloKind::Signed, &A, ProofKind::Proper, &A, true, &honest};
        const ClientResult r = run_client(env, true, sc);
        CHECK(r.obs.saw_client_hello);
        CHECK(r.obs.client_proved_identity);
        CHECK(r.obs.client_pub == client_key.pk);
        CHECK(honest.valid);
    }
    const Script strict_bad[] = {
        {"k1 anonymous server",                    Behaviour::Normal, HelloKind::Anonymous, nullptr, ProofKind::Zero,   nullptr, false},
        {"k2 hello key A, proof key B",            Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::Proper, &B,      false},
        {"k3 signed hello, anonymous proof",       Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::Zero,   nullptr, false},
        {"k4 proof over the wrong transcript",     Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::WrongTranscript, &A, false},
        {"k6 server reflects the client's hello",  Behaviour::ReflectHello, HelloKind::Signed, &A,   ProofKind::Proper, &A,      false},
    };
    for (const Script& sc : strict_bad) run_client(env, true, sc);

    // k5 replay: the whole honest server side of connection k0 (hello bytes and proof) played on a new connection.
    // The key and signatures are genuine; only the client's fresh nonce makes the transcript different.
    {
        Script sc{"k5 replay of an earlier honest session", Behaviour::Replay, HelloKind::Signed, &A, ProofKind::Proper, &A, false,
                  nullptr, &honest};
        if (honest.valid) run_client(env, true, sc);
    }

    // k7/k8/k9: silence. Two different clocks are involved and both are checked.
    //  - Silent peers trip the per-read idle timeout (about 3 s): the client must wait a little (it did not fail
    //    for some other reason) and must never exceed the 10 s absolute limit.
    //  - A peer that trickles one byte per second never trips the idle timeout; only the 10 s absolute deadline
    //    ends it. That deadline is fixed in the CLI (not configurable), so the bound is 9..14 s.
    struct Timed { Script script; double lo, hi; };
    const Timed timed[] = {
        {{"k7 silent server",             Behaviour::Silent,           HelloKind::Signed, &A, ProofKind::Proper, &A, false}, 1.5, 10.5},
        {{"k8 server silent after hello", Behaviour::SilentAfterHello, HelloKind::Signed, &A, ProofKind::Proper, &A, false}, 1.5, 10.5},
        {{"k9 server dribbling one byte per second", Behaviour::Dribble, HelloKind::Signed, &A, ProofKind::Proper, &A, false}, 9.0, 14.0},
    };
    for (const Timed& t : timed) {
        const ClientResult r = run_client(env, true, t.script);
        CHECK(r.seconds >= t.lo && r.seconds <= t.hi);
        if (r.seconds < t.lo || r.seconds > t.hi)
            std::cerr << "  " << t.script.name << ": the client took " << r.seconds << " s, expected " << t.lo << ".." << t.hi << " s\n";
    }

    std::cerr << "[client mode: compatibility]\n";
    const Script compat[] = {
        {"k10 hello key A, proof key B",        Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::Proper, &B,      false},
        {"k11 signed hello, anonymous proof",   Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::Zero,   nullptr, false},
        {"k12 proof over the wrong transcript", Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::WrongTranscript, &A, false},
        {"k13 fully anonymous server",          Behaviour::Normal, HelloKind::Anonymous, nullptr, ProofKind::Zero,   nullptr, true},
        {"k14 honest server",                   Behaviour::Normal, HelloKind::Signed,    &A,      ProofKind::Proper, &A,      true},
    };
    for (const Script& sc : compat) run_client(env, false, sc);
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

    const std::string addr_A = to_hex(address_from_pubkey(A.pk));
    const std::string addr_B = to_hex(address_from_pubkey(B.pk));

    // ---- PART 1: server mode ----
    scenario("strict", exe, chain_path, pin, chain, server_identity, true,
        {
            {"s1 anonymous hello",              HelloKind::Anonymous, nullptr, ProofKind::Zero,   nullptr, false},
            {"s2 signed hello, anonymous proof", HelloKind::Signed,    &A,      ProofKind::Zero,   nullptr, false},
            {"s3 proof by another key",         HelloKind::Signed,    &A,      ProofKind::Proper, &B,      false},
            {"s4 proof over wrong transcript",  HelloKind::Signed,    &A,      ProofKind::WrongTranscript, &A, false},
            {"s5 honest peer",                  HelloKind::Signed,    &A,      ProofKind::Proper, &A,      true},
        },
        /*served*/ 1, /*refused*/ 4, {addr_A});

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
        /*served*/ 3, /*refused*/ 4, {addr_A, addr_B});

    // ---- PART 2: client mode ----
    Key client_key;
    const std::string client_identity = tmpdir() + "/adv-client.key";
    write_text(client_identity, client_key.file_text());
    const ClientEnv env{exe, chain_path, pin, client_identity, &chain};
    client_scenarios(env, client_key, A, B);
    fs::remove(client_identity, ec);

    fs::remove(server_identity, ec);
    fs::remove(chain_path, ec);
    net_cleanup();
    if (failures == 0) { std::cout << "node adversarial tests: all passed (" << cases << " cases)\n"; return 0; }
    std::cerr << "node adversarial tests: " << failures << " failure(s)\n";
    return 1;
}
