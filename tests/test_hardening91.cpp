// v9.1: tests for the network problems found by reading and running v9.0 (see README "v9.1").
// H3, N2 and N3 FAIL on v9.0 (a legal block that cannot be synced); N1 and N4 guard fixes that were already on main.
// Each was run against the unfixed code to watch it fail before the fix was kept.
//
//   (SIGPIPE and the slow-peer deadline are covered in test_net.cpp, n06 and n07)
//   H3  the worst-case legal block fits in one message; a longer announced length is still refused
//   N1  a peer cannot jam a chain with a block stamped far in the future
//   N2  a legal block of 150 transfers (about 128 KB) can be synced
//   N3  a block larger than the sync byte budget is still accepted (once, then the budget stops the round)
//   N4  --transfer together with a peer mode is an error, not a silent no-op (already fixed on main; kept as a regression test)
// N1-N5 start the real taznode binary on every platform: POSIX uses "&", Windows uses cmd's "start /b".
#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/net.hpp"
#include "taz/persist.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>

#ifndef TAZ_NODE_BINARY
#error "TAZ_NODE_BINARY must be defined (see CMakeLists.txt)"
#endif

using namespace taz;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while (0)

struct Key { PrivKey sk{}; PubKey pk{}; Address addr{}; };
static Key new_key() { Key k; if (!generate_keypair(k.sk, k.pk)) std::exit(2); k.addr = address_from_pubkey(k.pk); return k; }
static Transaction transfer(const Key& f, const Address& to, Amount a, std::uint64_t n) {
    Transaction tx; tx.from = f.addr; tx.to = to; tx.amount = a; tx.fee = FEE_TRANSFER_V1; tx.nonce = n; tx.pubkey = f.pk;
    if (!sign(f.sk, tx.signable_bytes(), tx.signature)) std::exit(2);
    return tx;
}
static bool connected_pair(Listener& l, Socket& server, Socket& client, std::string& err) {
    return listen_loopback(0, l, &err) && connect_loopback(l.port, client, &err) && accept_one(l.socket, server, &err);
}
static std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }

// ---- H3 ---------------------------------------------------------------------------------------------------------
static void h3_worst_case_block_fits() {
    // A transaction with every variable-size field filled: five guardians, veto, new owner. Not a *valid* transaction
    // (nothing is checked here) - only a measure of the largest thing block_to_json can be asked to write per tx.
    Key k = new_key();
    Transaction fat = transfer(k, k.addr, 1, 0);
    fat.kind = TxKind::SetGuardians;
    for (int i = 0; i < 5; ++i) fat.guardians.push_back(new_key().addr);
    fat.guardian_threshold = 3; fat.veto = new_key().addr; fat.new_owner = new_key().addr;
    fat.nonce = std::numeric_limits<std::uint64_t>::max(); fat.amount = std::numeric_limits<Amount>::max();
    Block b; b.height = std::numeric_limits<std::uint64_t>::max(); b.timestamp = std::numeric_limits<std::int64_t>::max();
    b.transactions.assign(MAX_BLOCK_TXS, fat);
    const std::size_t size = block_to_json(b).size();
    std::cout << "  worst-case block: " << MAX_BLOCK_TXS << " transactions = " << size << " bytes of JSON, transport limit " << kMaxPayloadBytes << "\n";
    CHECK(size <= kMaxPayloadBytes);
    { Block back; std::string why;                                // ...and the parser must accept what the writer wrote
      const bool parsed = parse_block_text(block_to_json(b), back, &why);
      if (!parsed) std::cerr << "  worst-case block does not parse: " << why << "\n";
      CHECK(parsed); CHECK(parsed && back.hash() == b.hash()); }
    // and a longer announced length is still refused before anything is read
    std::string err; Listener l; Socket server, client;
    PlainTransport client_t(client);
    CHECK(connected_pair(l, server, client, err));
    const std::uint32_t over = kMaxPayloadBytes + 1;
    std::string header; for (int i = 3; i >= 0; --i) header.push_back(static_cast<char>((over >> (8 * i)) & 0xff));
    header.push_back(static_cast<char>(kMsgBlocks));
    CHECK(send_all(server, header, &err));
    std::uint8_t type = 0; std::string payload;
    CHECK(!recv_message(client_t, type, payload, &err));
    CHECK(err.find("message too large") != std::string::npos);
    // a big (3 MiB) message still arrives intact (on a fresh connection: the refused one is dropped, as a real node does)
    Listener l2; Socket server2, client2;
    PlainTransport server2_t(server2), client2_t(client2);
    CHECK(connected_pair(l2, server2, client2, err));
    const std::string big(3u * 1024u * 1024u, 'q');
    std::thread sender([&] { std::string e; send_message(server2_t, kMsgBlocks, big, &e); });
    const bool got_big = recv_message(client2_t, type, payload, &err);
    if (!got_big) std::cerr << "  big message: " << err << "\n";
    CHECK(got_big);
    sender.join();
    CHECK(payload == big && type == kMsgBlocks);
}

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
    return std::system(full.c_str());
}
static std::string pin(const Chain& c) { return to_hex(c.genesis_commitment()); }
static bool wait_for(const fs::path& file, const std::string& text, int seconds) {
    for (int i = 0; i < seconds * 20; ++i) {
        if (slurp(file).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
// Runs: A listens (from a genesis-only file), B (the longer chain) connects. Returns A's output; A's file is read back after.
// expect_served=false is for the tests where the peer must be refused: since v9.1 a refused peer is not a served
// peer, so the node keeps waiting instead of counting it.
static std::string sync_run(const fs::path& dir, const Chain& a, const Chain& b, bool expect_served = true) {
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(a, (dir / "A.json").string(), &err));
    CHECK(save_chain_json(b, (dir / "B.json").string(), &err));
    const std::uint16_t port = free_port();
    const std::string out = (dir / "a.out").string();
    // --serve-timeout: a node waiting for a peer that never comes would outlive this test on Windows, where
    // nothing kills a background process for us.
    const std::string a_cmd = node() + " --chain \"" + (dir / "A.json").string() + "\" --expect-genesis " + pin(a) +
                              " --listen " + std::to_string(port) + " --serve-timeout 40";
    CHECK(run_in_background(a_cmd, out));
    CHECK(wait_for(out, "listening", 10));
    const std::string b_cmd = node() + " --chain \"" + (dir / "B.json").string() + "\" --expect-genesis " + pin(b) + " --peer " + std::to_string(port);
    const int b_rc = run_waiting(b_cmd, (dir / "b.out").string());
    if (b_rc != 0) std::cerr << "  (the connecting node exited with " << b_rc << ")\n";
    const std::string wanted_line = expect_served ? "served 1 of 1" : "still waiting for 1 peer(s)";
    if (!wait_for(out, wanted_line, 20)) {
        std::cerr << "  the serving node never reported \"" << wanted_line << "\". Its output:\n" << slurp(out) << "\n";
        std::cerr << "  the connecting node said:\n" << slurp(dir / "b.out") << "\n";
    }
    CHECK(wait_for(out, wanted_line, 20));
    return slurp(out);
}
static std::uint64_t height_of(const fs::path& file, const Chain& like) {
    Chain c; LoadOptions o; o.require_genesis_pin = true; o.expected_genesis_commitment = like.genesis_commitment();
    std::string err;
    if (!load_chain_json(file.string(), c, &err, o)) { std::cerr << "cannot load " << file << ": " << err << "\n"; return 9999999; }
    return c.height();
}
static Chain two_account_chain(const Key& a, const Key& b, Amount balance) {
    Chain c; if (!c.init_genesis(a.addr, balance, b.addr, 0)) std::exit(2); return c;
}
static void add_transfers(Chain& c, const Key& from, const Key& to, std::uint64_t count) {
    std::vector<Transaction> txs; for (std::uint64_t i = 0; i < count; ++i) txs.push_back(transfer(from, to.addr, 1, i));
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const ChainResult r = c.append_block(txs, now - 10);
    if (!r.ok) { std::cerr << "setup failed: " << r.error << "\n"; std::exit(2); }
}

// ---- F1: a node more than 1000 blocks behind catches up over bounded polling rounds ----------------------------
static void f1_syncs_when_peer_is_more_than_1000_blocks_ahead() {
    Key a = new_key(), b = new_key();
    Chain behind = two_account_chain(a, b, 4000000000000ULL);
    Chain ahead = behind;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Keep every block timestamp inside the receiver's +120s clock allowance while creating a 1001-block gap.
    const std::int64_t first_stamp = now - 1100;
    for (std::uint64_t i = 0; i < 1001; ++i) {
        const Transaction tx = transfer(a, b.addr, 1, i);
        const ChainResult r = ahead.append_block({tx}, first_stamp + static_cast<std::int64_t>(i));
        if (!r.ok) { std::cerr << "F1 setup failed at block " << (i + 1) << ": " << r.error << "\n"; std::exit(2); }
    }
    CHECK(ahead.height() == 1001);
    const fs::path dir = fs::temp_directory_path() / "taz-f1-over-1000";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(behind, (dir / "behind.json").string(), &err));
    CHECK(save_chain_json(ahead, (dir / "ahead.json").string(), &err));
    const std::uint16_t port = free_port();
    const std::string server_out = (dir / "server.out").string();
    const std::string chain_arg = " --chain \"" + (dir / "behind.json").string() + "\" --expect-genesis " + pin(behind);
    const std::string server = node() + chain_arg + " --serve " + std::to_string(port) +
                               " --max-peers 6 --serve-timeout 60";
    CHECK(run_in_background(server, server_out));
    CHECK(wait_for(server_out, "listening", 10));
    // Six successful connections are needed: five batches of 200 and one batch of 1.
    // The seventh attempt sees the server stop after its six-peer quota; retries=1 makes the poller exit.
    const std::string client = node() + " --chain \"" + (dir / "ahead.json").string() +
                               "\" --expect-genesis " + pin(ahead) + " --peer " + std::to_string(port) +
                               " --every 1 --retries 1";
    (void)run_waiting(client, (dir / "client.out").string());
    if (!wait_for(server_out, "served 6 of 6", 60)) {
        std::cerr << "  F1 server output:\\n" << slurp(server_out) << "\n";
        std::cerr << "  F1 client output:\\n" << slurp(dir / "client.out") << "\n";
    }
    CHECK(wait_for(server_out, "served 6 of 6", 1));
    CHECK(height_of(dir / "behind.json", behind) == 1001);
}

static void n1_future_block_cannot_jam_the_chain() {
    Key a = new_key(), b = new_key();
    Chain good = two_account_chain(a, b, 1000000000);
    Chain poisoned = good;
    Block blk; blk.height = 1; blk.previous_hash = poisoned.blocks().back().hash();
    blk.timestamp = std::numeric_limits<std::int64_t>::max(); blk.state_root = poisoned.state().state_root();
    CHECK(poisoned.accept_block(blk).ok);                         // the library itself has no opinion about the clock
    const fs::path dir = fs::temp_directory_path() / "taz91-n1";
    const std::string a_out = sync_run(dir, good, poisoned, false);   // refused, so not counted as served
    CHECK(a_out.find("cannot accept") != std::string::npos);
    CHECK(a_out.find("too far in the future") != std::string::npos);
    CHECK(height_of(dir / "A.json", good) == 0);                  // v9.0: height 1, and no honest block can ever follow
}
static void n2_a_150_transfer_block_syncs() {
    Key a = new_key(), b = new_key();
    Chain good = two_account_chain(a, b, 4000000000000ULL);
    Chain ahead = good; add_transfers(ahead, a, b, 150);
    CHECK(block_to_json(ahead.blocks().back()).size() > 64 * 1024);   // would not have fitted the v9.0 limit
    const fs::path dir = fs::temp_directory_path() / "taz91-n2";
    const std::string a_out = sync_run(dir, good, ahead);
    CHECK(a_out.find("accepted 1 block") != std::string::npos);
    CHECK(height_of(dir / "A.json", good) == 1);
}
static void n3_block_over_the_byte_budget_still_syncs() {
    Key a = new_key(), b = new_key();
    Chain good = two_account_chain(a, b, 4000000000000ULL);
    Chain ahead = good; add_transfers(ahead, a, b, 5300);
    const std::size_t size = block_to_json(ahead.blocks().back()).size();
    std::cout << "  budget test block: 5300 transfers = " << size << " bytes (sync byte budget is 4 MiB)\n";
    CHECK(size > 4u * 1024u * 1024u);
    const fs::path dir = fs::temp_directory_path() / "taz91-n3";
    const std::string a_out = sync_run(dir, good, ahead);
    CHECK(a_out.find("accepted 1 block") != std::string::npos);   // v9.0 read it, dropped it, and repeated that every round
    CHECK(height_of(dir / "A.json", good) == 1);
}
static void n4_transfer_with_peer_is_an_error() {
    Key a = new_key(), b = new_key();
    Chain c = two_account_chain(a, b, 1000000);
    const fs::path dir = fs::temp_directory_path() / "taz91-n4";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err; CHECK(save_chain_json(c, (dir / "c.json").string(), &err));
    std::ofstream(dir / "key.txt") << to_hex(a.sk) << "\n" << to_hex(a.pk) << "\n";
    const std::string cmd = node() + " --chain \"" + (dir / "c.json").string() + "\" --expect-genesis " + pin(c) + " --key-file \"" + (dir / "key.txt").string()
                          + "\" --transfer " + to_hex(b.addr) + ":5 --peer 1";
    const int rc = run_waiting(cmd, (dir / "o.txt").string());
    CHECK(rc != 0);                                               // v9.0: exit 0, nothing produced
    CHECK(slurp(dir / "o.txt").find("cannot be combined") != std::string::npos);
}
// v9.1: --require-identity against a real node process. An anonymous peer must be refused, the node must keep
// waiting, and a peer that proves its key must then be accepted — that is the whole point of the flag.
static void n5_require_identity_refuses_anonymous_and_keeps_serving() {
    Key a = new_key(), b = new_key(), id = new_key();
    Chain c = two_account_chain(a, b, 1000000);
    const fs::path dir = fs::temp_directory_path() / "taz91-n5";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(c, (dir / "c.json").string(), &err));
    std::ofstream(dir / "id.txt") << to_hex(id.sk) << "\n" << to_hex(id.pk) << "\n";

    const std::uint16_t port = free_port();
    const std::string out = (dir / "a.out").string();
    const std::string chain_arg = " --chain \"" + (dir / "c.json").string() + "\" --expect-genesis " + pin(c);
    const std::string server = node() + chain_arg + " --listen " + std::to_string(port) +
                               " --require-identity --identity \"" + (dir / "id.txt").string() + "\" --serve-timeout 40";
    CHECK(run_in_background(server, out));
    CHECK(wait_for(out, "listening", 10));

    // an anonymous peer is refused, and the node does not exit
    const std::string anon = node() + chain_arg + " --peer " + std::to_string(port);
    // Now that the accepter confirms before syncing, a refused peer must notice and fail.
    CHECK(run_waiting(anon, (dir / "anon.out").string()) != 0);
    if (!wait_for(out, "did not prove an identity", 10)) std::cerr << "  server output:\n" << slurp(out) << "\n";
    CHECK(wait_for(out, "did not prove an identity", 10));
    CHECK(wait_for(out, "still waiting for", 10));

    // a peer that proves its key is accepted
    const std::string signed_peer = node() + chain_arg + " --peer " + std::to_string(port) +
                                    " --identity \"" + (dir / "id.txt").string() + "\"";
    if (run_waiting(signed_peer, (dir / "ok.out").string()) != 0) std::cerr << "  (the proving node exited non-zero)\n";
    CHECK(wait_for(out, "served 1 of 1", 20));
    CHECK(slurp(out).find("proof verified on this connection") != std::string::npos);
}

// F5: if the peer's tip cannot attach to our local history, this is an unrepaired deep fork.
// The node must refuse the sync (non-zero), not report a successful round while silently keeping its chain.
// Common-ancestor reorganization is deliberately out of scope for this PR.
static void f5_deep_fork_must_not_return_success() {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const std::uint64_t depth : {2ULL, 3ULL, 10ULL}) {
        Key a = new_key(), b = new_key();
        Chain local = two_account_chain(a, b, 1000000000);
        Chain remote = local;
        for (std::uint64_t h = 1; h <= depth; ++h) {
            const std::int64_t stamp = now - 100 + static_cast<std::int64_t>(h);
            const Amount local_amount = (h == 1 ? 1 : 1);
            const Amount remote_amount = (h == 1 ? 2 : 1);
            const ChainResult left = local.append_block({transfer(a, b.addr, local_amount, h - 1)}, stamp);
            const ChainResult right = remote.append_block({transfer(a, b.addr, remote_amount, h - 1)}, stamp);
            if (!left.ok || !right.ok) {
                std::cerr << "F5 setup failed at depth " << depth << ": "
                          << left.error << " / " << right.error << "\n";
                std::exit(2);
            }
        }
        CHECK(local.height() == depth);
        CHECK(remote.height() == depth);
        CHECK(local.blocks().back().hash() != remote.blocks().back().hash());

        const fs::path dir = fs::temp_directory_path() / ("taz-f5-depth-" + std::to_string(depth));
        std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
        std::string err;
        CHECK(save_chain_json(local, (dir / "local.json").string(), &err));
        CHECK(save_chain_json(remote, (dir / "remote.json").string(), &err));
        const std::uint16_t port = free_port();
        const std::string server_out = (dir / "server.out").string();
        const std::string client_out = (dir / "client.out").string();
        const std::string server = node() + " --chain \"" + (dir / "local.json").string() +
            "\" --expect-genesis " + pin(local) + " --listen " + std::to_string(port) +
            " --serve-timeout 2";
        CHECK(run_in_background(server, server_out));
        CHECK(wait_for(server_out, "listening", 10));
        const std::string client = node() + " --chain \"" + (dir / "remote.json").string() +
            "\" --expect-genesis " + pin(remote) + " --peer " + std::to_string(port);
        const int client_rc = run_waiting(client, client_out);
        CHECK(client_rc != 0); // current code returns 0 after silently keeping the local chain
        CHECK(slurp(client_out).find("cannot attach peer tip") != std::string::npos);
        CHECK(wait_for(server_out, "refused one connection", 5));
        CHECK(slurp(server_out).find("served 1 of 1") == std::string::npos);
        CHECK(height_of(dir / "local.json", local) == depth);
        CHECK(height_of(dir / "remote.json", remote) == depth);
        fs::remove_all(dir, ec);
    }
}

// v9.2: --serve-timeout must end a node that is waiting for a peer that never comes. Without it the process
// outlives the test on Windows, which is exactly why these tests were off there.
static void n6_serve_timeout_ends_the_node() {
    Key a = new_key(), b = new_key();
    Chain c = two_account_chain(a, b, 1000000);
    const fs::path dir = fs::temp_directory_path() / "taz91-n6";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    std::string err;
    CHECK(save_chain_json(c, (dir / "c.json").string(), &err));

    const std::uint16_t port = free_port();
    const std::string out = (dir / "o.txt").string();
    const std::string cmd = node() + " --chain \"" + (dir / "c.json").string() + "\" --expect-genesis " + pin(c) +
                            " --listen " + std::to_string(port) + " --serve-timeout 3";
    const auto start = std::chrono::steady_clock::now();
    const int rc = run_waiting(cmd, out);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

    CHECK(rc == 0);                                   // running out of time is not a failure
    CHECK(elapsed >= 2);                              // it really waited
    CHECK(elapsed <= 40);                             // and it really stopped
    CHECK(slurp(out).find("stopped waiting after 3s") != std::string::npos);
    std::cout << "  a node with --serve-timeout 3 stopped after " << elapsed << "s\n";
}

int main() {
    { std::string e; if (!net_startup(&e)) { std::cerr << "net_startup failed: " << e << "\n"; return 1; } }   // Windows needs WSAStartup first
    h3_worst_case_block_fits();
    // N1-N5 start the real taznode. They run on Windows too now. The reason they were off was not a Windows bug:
    // a node waiting for a peer that never arrived outlived the test, so the run hung. --serve-timeout removes
    // that, and the launcher is checked by waiting for the node's own "listening" line rather than by trusting
    // that it started.
    {
        f1_syncs_when_peer_is_more_than_1000_blocks_ahead();
        n1_future_block_cannot_jam_the_chain();
        n2_a_150_transfer_block_syncs();
        n3_block_over_the_byte_budget_still_syncs();
        n4_transfer_with_peer_is_an_error();
        n5_require_identity_refuses_anonymous_and_keeps_serving();
        n6_serve_timeout_ends_the_node();
        f5_deep_fork_must_not_return_success();
    }
    std::error_code ec;
    if (std::getenv("TAZ_KEEP_TMP") == nullptr) for (const char* d : {"taz-f1-over-1000", "taz91-n1", "taz91-n2", "taz91-n3", "taz91-n4", "taz91-n5", "taz91-n6"}) fs::remove_all(fs::temp_directory_path() / d, ec);
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "Hardening v9.1 tests passed. This is not a security audit.\n";
    return EXIT_SUCCESS;
}
