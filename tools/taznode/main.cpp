// taznode — the first, deliberately small, node binary.
//
// What it does today: reads its configuration, loads the chain through load_chain_at_startup() (which refuses
// to load without a trusted genesis commitment), and prints what it loaded so an operator can compare the
// genesis commitment with the release notes.
//
// What it does NOT do: create a genesis, produce blocks, talk to other nodes, or listen on a socket. Those are
// the next steps, and none of them is pretended here.
#include "taz/startup.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include "taz/net.hpp"
#include "taz/fork.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: taznode --chain <file> --expect-genesis <64 hex>\n"
    "       taznode --chain <file> --init-genesis <40 hex address>:<amount> --init-genesis <40 hex>:<amount>\n"
    "\n"
    "  --chain <file>            the chain file to load or create\n"
    "  --expect-genesis <hex>    the genesis commitment you trust, taken from outside the file\n"
    "  --init-genesis <a>:<n>    create a new chain with these two genesis accounts (use exactly twice)\n"
    "  --key-file <path>         prototype key file: line 1 = 64 hex private seed, line 2 = 64 hex public key\n"
    "  --transfer <a>:<n>        produce one block with a transfer from the key's address to <a>\n"
    "  --listen <port>           wait for one peer on 127.0.0.1:<port>, sync, then stop\n"
    "  --serve <port>            stay up: accept peers one at a time (see --max-peers)\n"
    "  --max-peers <n>           how many peers --serve accepts before stopping (default 5)\n"
    "  --peer <port>             connect to 127.0.0.1:<port>, sync, then stop\n"
    "  --every <seconds>         with --peer: keep reconnecting at that interval (poll gossip)\n"
    "  --help                    this text\n"
    "\n"
    "Loading a node that cannot name its chain does not start: without --expect-genesis it exits non-zero.\n"
    "Creating a chain needs no pin, because there is nothing yet to trust; it prints the commitment it made.\n"
    "Creation never overwrites: if the chain file already exists, it exits non-zero and leaves it alone.\n";

int usage_error(const std::string& why) {
    std::cerr << "taznode: " << why << "\n\n" << kUsage;
    return 2;
}

// from_hex() maps every character it is given, including ones that are not hex at all ("z" becomes a byte),
// so every caller must check the text first. This is that check.
bool is_hex(const std::string& s) {
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}
bool is_hex64(const std::string& s) { return s.size() == 64 && is_hex(s); }

std::vector<std::uint8_t> from_hex(const std::string& s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((nib(s[i]) << 4) | nib(s[i + 1])));
    return out;
}

bool parse_address(const std::string& s, taz::Address& out) {
    if (s.size() != 40 || !is_hex(s)) return false;
    const std::vector<std::uint8_t> raw = from_hex(s);
    if (raw.size() != out.size()) return false;
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = raw[i];
    return true;
}

// Reads "seed hex" on line 1 and "public key hex" on line 2. Prototype only: a real node must keep keys in
// memory it controls, not in a file next to the chain. Reading both means the node never has to derive the
// public key from the seed, which the crypto layer does not expose today.
bool read_key_file(const std::string& path, taz::PrivKey& sk, taz::PubKey& pk, std::string& why) {
    std::ifstream in(path);
    if (!in) { why = "cannot open the key file"; return false; }
    std::string seed_hex, pub_hex;
    std::getline(in, seed_hex);
    std::getline(in, pub_hex);
    auto trim = [](std::string& v) {
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
        std::size_t i = 0;
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        v.erase(0, i);
    };
    trim(seed_hex); trim(pub_hex);
    if (seed_hex.size() != 64 || pub_hex.size() != 64 || !is_hex(seed_hex) || !is_hex(pub_hex)) {
        why = "the key file needs 64 hex characters on each of two lines";
        return false;
    }
    std::vector<std::uint8_t> raw_seed = from_hex(seed_hex);   // not const: it is wiped below
    const std::vector<std::uint8_t> raw_pub = from_hex(pub_hex);
    if (raw_seed.size() != sk.size() || raw_pub.size() != pk.size()) { why = "the key file is not hex"; return false; }
    for (std::size_t i = 0; i < sk.size(); ++i) sk[i] = raw_seed[i];
    for (std::size_t i = 0; i < pk.size(); ++i) pk[i] = raw_pub[i];
    // Wipe the originals. The earlier version copied raw_seed into a local, wiped the copy, and left the real
    // bytes in memory — a wipe that looks right and does nothing.
    taz::secure_wipe(raw_seed);
    std::fill(seed_hex.begin(), seed_hex.end(), '\0');
    return true;
}

bool parse_amount(const std::string& s, taz::Amount& out) {
    if (s.empty() || s.size() > 19) return false;
    taz::Amount v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<taz::Amount>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string chain_file;
    std::string expect_hex;
    std::vector<std::string> init_specs;
    std::string key_file;
    std::string transfer_spec;
    std::string listen_port;
    std::string serve_port;
    std::string max_peers_text;
    std::string every_text;
    std::string peer_port;
    bool seen_chain = false, seen_pin = false, seen_key = false, seen_transfer = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { std::cout << kUsage; return 0; }
        // A flag given twice is an operator mistake; silently taking the last one hides it.
        if (a == "--chain" && i + 1 < argc) {
            if (seen_chain) return usage_error("--chain was given twice");
            seen_chain = true; chain_file = argv[++i]; continue;
        }
        if (a == "--expect-genesis" && i + 1 < argc) {
            if (seen_pin) return usage_error("--expect-genesis was given twice");
            seen_pin = true; expect_hex = argv[++i]; continue;
        }
        if (a == "--init-genesis" && i + 1 < argc) { init_specs.push_back(argv[++i]); continue; }
        if (a == "--key-file" && i + 1 < argc) {
            if (seen_key) return usage_error("--key-file was given twice");
            seen_key = true; key_file = argv[++i]; continue;
        }
        if (a == "--listen" && i + 1 < argc) { listen_port = argv[++i]; continue; }
        if (a == "--serve" && i + 1 < argc) { serve_port = argv[++i]; continue; }
        if (a == "--max-peers" && i + 1 < argc) { max_peers_text = argv[++i]; continue; }
        if (a == "--every" && i + 1 < argc) { every_text = argv[++i]; continue; }
        if (a == "--peer" && i + 1 < argc) { peer_port = argv[++i]; continue; }
        if (a == "--transfer" && i + 1 < argc) {
            if (seen_transfer) return usage_error("--transfer was given twice");
            seen_transfer = true; transfer_spec = argv[++i]; continue;
        }
        return usage_error("unknown argument \"" + a + "\"");
    }
    if (chain_file.empty()) return usage_error("--chain is required");

    // Creating a chain: no pin is needed, because there is nothing to trust yet. The commitment this prints is
    // what an operator hands to everyone else, and what this node will demand on every later start.
    if (!init_specs.empty()) {
        if (init_specs.size() != 2) return usage_error("--init-genesis must be given exactly twice");
        taz::Address a0{}, a1{};
        taz::Amount n0 = 0, n1 = 0;
        const std::size_t c0 = init_specs[0].find(':');
        const std::size_t c1 = init_specs[1].find(':');
        if (c0 == std::string::npos || c1 == std::string::npos) return usage_error("--init-genesis wants <address>:<amount>");
        if (!parse_address(init_specs[0].substr(0, c0), a0) || !parse_address(init_specs[1].substr(0, c1), a1))
            return usage_error("a genesis address must be 40 hex characters");
        if (!parse_amount(init_specs[0].substr(c0 + 1), n0) || !parse_amount(init_specs[1].substr(c1 + 1), n1))
            return usage_error("a genesis amount must be a decimal number");
        // Never replace an existing chain. A bootstrap command that quietly overwrites is a way to destroy the
        // only copy of a chain, and the operator would find out afterwards. Check first, say why, exit non-zero.
        // (This is a check-then-write, so it is not atomic against another process writing the same path at the
        // same instant; that is acceptable for a one-off bootstrap command, and it is not a security boundary.)
        std::error_code exists_ec;
        if (std::filesystem::exists(chain_file, exists_ec)) {
            std::cerr << "taznode: refusing to create " << chain_file
                      << ": the file already exists and will not be replaced\n";
            return 4;
        }
        taz::Chain fresh;
        if (!fresh.init_genesis(a0, n0, a1, n1)) return usage_error("genesis could not be created (same address twice?)");
        std::string save_err;
        if (!taz::save_chain_json(fresh, chain_file, &save_err)) {
            std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
            return 3;
        }
        std::cout << "taznode: created chain=" << chain_file
                  << " height=" << fresh.height()
                  << " genesis=" << taz::to_hex(fresh.genesis_commitment()) << "\n";
        return 0;
    }
    if (expect_hex.empty()) return usage_error("--expect-genesis is required: a node must be able to name its chain");
    if (!is_hex64(expect_hex)) return usage_error("--expect-genesis must be 64 hex characters");

    const std::vector<std::uint8_t> raw = from_hex(expect_hex);
    taz::Hash32 pin{};
    if (raw.size() != pin.size()) return usage_error("--expect-genesis must be 64 hex characters");
    for (std::size_t i = 0; i < pin.size(); ++i) pin[i] = raw[i];

    taz::StartupConfig cfg;
    cfg.chain_file = chain_file;
    cfg.trusted_genesis = pin;

    taz::Chain chain;
    std::string err;
    const taz::StartupResult r = taz::load_chain_at_startup(cfg, chain, &err);
    if (!r.ok) {
        std::cerr << "taznode: " << (err.empty() ? r.error : err) << "\n";
        return 3;                                   // refused: wrong chain, unreadable file, or no pin
    }

    // One block an operator can compare with the release notes without opening a file.
    std::cout << "taznode: loaded"
              << " chain_id=" << taz::CHAIN_ID
              << " protocol=" << taz::PROTOCOL_VERSION
              << " core=" << taz::CORE_VERSION
              << " height=" << chain.height()
              << " genesis=" << taz::to_hex(chain.genesis_commitment())
              << " state_root=" << taz::to_hex(chain.state().state_root())
              << "\n";

    // ---- talk to peers ----
    // Deliberately small: one peer at a time, no threads, no peer list, no discovery. --listen serves exactly one
    // peer and stops; --serve keeps accepting, one after another, up to --max-peers. Both use the same per-peer
    // routine, so the rules cannot drift between the two modes.
    // Checked before the mode block: "--max-peers 3" on its own is a mistake whether or not a mode was given,
    // and skipping the block would have let it through as a plain load.
    if (!max_peers_text.empty() && serve_port.empty()) return usage_error("--max-peers only means something with --serve");
    if (!every_text.empty() && peer_port.empty()) return usage_error("--every only means something with --peer");
    if (!listen_port.empty() || !serve_port.empty() || !peer_port.empty()) {
        const int given = (!listen_port.empty() ? 1 : 0) + (!serve_port.empty() ? 1 : 0) + (!peer_port.empty() ? 1 : 0);
        if (given > 1) return usage_error("--listen, --serve and --peer are three different modes; pick one");

        auto parse_port = [](const std::string& text, std::uint16_t& out) -> bool {
            taz::Amount value = 0;
            if (!parse_amount(text, value) || value == 0 || value > 65535) return false;
            out = static_cast<std::uint16_t>(value);
            return true;
        };
        std::uint16_t port = 0;
        const std::string& port_text = !listen_port.empty() ? listen_port : (!serve_port.empty() ? serve_port : peer_port);
        if (!parse_port(port_text, port)) return usage_error("the port must be a number between 1 and 65535");

        std::uint64_t max_peers = 5;
        if (!max_peers_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(max_peers_text, value) || value == 0 || value > 1000)
                return usage_error("--max-peers must be between 1 and 1000");
            max_peers = value;
        }

        std::string net_err;
        if (!taz::net_startup(&net_err)) return usage_error("network start failed: " + net_err);

        // Built per peer, not once: after adopting a peer's chain our height changes, and telling the next peer
        // the old height would be a lie it acts on.

        auto offer_chain = [&](taz::Socket& sock) -> bool {
            const std::string text = taz::to_json(chain);
            if (text.size() > taz::kMaxPayloadBytes) {
                std::cerr << "taznode: our chain is larger than one message; multi-message sync is not built yet\n";
                return false;
            }
            if (!taz::send_message(sock, taz::kMsgChain, text, &net_err)) {
                std::cerr << "taznode: " << net_err << "\n";
                return false;
            }
            std::cout << "taznode: offered our chain height=" << chain.height() << " bytes=" << text.size() << "\n";
            return true;
        };
        auto receive_chain = [&](taz::Socket& sock) -> bool {
            std::uint8_t type = 0;
            std::string payload;
            if (!taz::recv_message(sock, type, payload, &net_err)) {
                std::cerr << "taznode: " << net_err << "\n";
                return false;
            }
            if (type != taz::kMsgChain) {
                std::cerr << "taznode: the peer did not offer a chain\n";
                return false;
            }
            taz::Chain incoming;
            std::string load_err;
            if (!taz::load_chain_json_text(payload, incoming, &load_err)) {
                std::cerr << "taznode: the peer's chain is not valid: " << load_err << "\n";
                return false;
            }
            constexpr std::uint64_t kMaxSyncAhead = 1000;
            if (incoming.height() > chain.height() + kMaxSyncAhead) {
                std::cerr << "taznode: the peer's chain is " << (incoming.height() - chain.height())
                          << " blocks ahead; more than " << kMaxSyncAhead << " is refused\n";
                return false;
            }
            const taz::ForkDecision fork = taz::choose_chain(chain, incoming);
            if (fork.choice == taz::ForkChoice::DifferentChain) {
                std::cerr << "taznode: the peer's chain has a different genesis; refused\n";
                return false;
            }
            if (fork.choice == taz::ForkChoice::SameChain) {
                std::cout << "taznode: the peer has the same chain (height " << chain.height() << ")\n";
                return true;
            }
            if (fork.choice == taz::ForkChoice::KeepMine) {
                std::cout << "taznode: kept our chain (" << fork.reason << ", height " << chain.height()
                          << ", peer offered " << incoming.height() << ")\n";
                if (incoming.height() == chain.height())
                    std::cerr << "taznode: warning: the peer disagrees at the same height - possible fork\n";
                return true;
            }
            std::string save_err;
            if (!taz::save_chain_json(incoming, chain_file, &save_err)) {
                std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
                return false;
            }
            std::cout << "taznode: adopted the peer's chain height=" << incoming.height()
                      << " (" << fork.reason << ")\n";
            chain = std::move(incoming);
            return true;
        };
        // One conversation with one peer: handshake, decide, sync both ways. Returns false when the peer is not
        // on our chain or misbehaves; the caller decides whether that ends the program or just this peer.
        auto handle_peer = [&](taz::Socket& sock, bool we_speak_first) -> bool {
            const taz::Hello ours = taz::hello_for(chain);   // current height, every time
            taz::Hello theirs;
            if (!taz::exchange_hello(sock, ours, theirs, we_speak_first, &net_err)) {
                std::cerr << "taznode: the handshake failed: " << net_err << "\n";
                return false;
            }
            const taz::HandshakeDecision decision = taz::check_peer(ours, theirs);
            if (!decision.ok) {
                std::cerr << "taznode: peer refused: " << decision.reason << "\n";
                return false;
            }
            std::cout << "taznode: peer accepted chain_id=" << theirs.chain_id
                      << " protocol=" << theirs.protocol_version
                      << " height=" << theirs.best_height
                      << " genesis=" << taz::to_hex(theirs.genesis_commitment) << "\n";
            // Refuse before receiving: the hello already states the peer's height, and loading a chain replays
            // every block. Checking after the load spends the CPU the limit exists to save.
            constexpr std::uint64_t kMaxSyncAhead = 1000;
            if (theirs.best_height > chain.height() + kMaxSyncAhead) {
                std::cerr << "taznode: the peer claims to be " << (theirs.best_height - chain.height())
                          << " blocks ahead; more than " << kMaxSyncAhead << " is refused before syncing\n";
                return false;
            }
            return we_speak_first ? (offer_chain(sock) && receive_chain(sock))
                                  : (receive_chain(sock) && offer_chain(sock));
        };

        std::uint64_t every = 0;
        if (!every_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(every_text, value) || value == 0 || value > 3600)
                return usage_error("--every must be a number of seconds between 1 and 3600");
            every = value;
        }

        bool all_ok = true;
        if (!peer_port.empty()) {
            // Poll gossip: without threads this node cannot be pushed to, so it pulls. Connect, sync, close,
            // wait, repeat. It is not gossip in the academic sense and it does not scale, but it means a new
            // block reaches the other node without anyone typing a command — which is the point.
            std::uint64_t round = 0;
            do {
                ++round;
                taz::Socket socket;
                if (!taz::connect_loopback(port, socket, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    all_ok = false;
                    break;
                }
                all_ok = handle_peer(socket, true);
                socket.close();
                if (every != 0 && all_ok) {
                    std::cout << "taznode: round " << round << " done, our height is " << chain.height()
                              << "; waiting " << every << "s\n";
                    std::this_thread::sleep_for(std::chrono::seconds(every));
                }
            } while (every != 0 && all_ok);
        } else {
            taz::Listener listener;
            if (!taz::listen_loopback(port, listener, &net_err)) {
                std::cerr << "taznode: " << net_err << "\n";
                taz::net_cleanup();
                return 3;
            }
            const std::uint64_t wanted = listen_port.empty() ? max_peers : 1;
            std::cout << "taznode: listening on 127.0.0.1:" << listener.port
                      << " - will serve " << wanted << " peer" << (wanted == 1 ? "" : "s") << "\n";
            for (std::uint64_t served = 0; served < wanted; ++served) {
                taz::Socket peer;
                if (!taz::accept_one(listener.socket, peer, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    all_ok = false;
                    break;
                }
                // A peer that is not on our chain ends its own conversation, not the node.
                if (!handle_peer(peer, false)) all_ok = false;
                std::cout << "taznode: served " << (served + 1) << " of " << wanted
                          << ", our height is now " << chain.height() << "\n";
            }
        }
        taz::net_cleanup();
        return all_ok ? 0 : 3;
    }

    // ---- block production: one transfer, one block, then save ----
    if (!transfer_spec.empty()) {
        if (key_file.empty()) return usage_error("--transfer needs --key-file: a transaction must be signed by its sender");
        const std::size_t colon = transfer_spec.find(':');
        if (colon == std::string::npos) return usage_error("--transfer wants <address>:<amount>");
        taz::Address to{};
        taz::Amount amount = 0;
        if (!parse_address(transfer_spec.substr(0, colon), to)) return usage_error("the transfer address must be 40 hex characters");
        if (!parse_amount(transfer_spec.substr(colon + 1), amount)) return usage_error("the transfer amount must be a decimal number");

        taz::PrivKey sk{};
        taz::PubKey pk{};
        std::string why;
        if (!read_key_file(key_file, sk, pk, why)) return usage_error(why);
        const taz::Address from = taz::address_from_pubkey(pk);

        taz::Transaction tx;
        tx.from = from;
        tx.to = to;
        tx.amount = amount;
        tx.fee = taz::FEE_TRANSFER_V1;
        tx.nonce = chain.state().nonce(from);
        tx.pubkey = pk;
        const std::vector<std::uint8_t> signable = tx.signable_bytes();
        if (!taz::sign(sk, signable, tx.signature)) return usage_error("the key cannot sign (is the key file right?)");
        // The file carries the seed and the public key side by side, so they can disagree. Signing with one and
        // claiming the other would produce a transaction that fails much later with a confusing message; catch
        // it here instead.
        if (!taz::verify(pk, signable, tx.signature))
            return usage_error("the key file is inconsistent: that public key does not belong to that seed");

        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count();
        // The chain requires a timestamp strictly greater than the previous block. Two blocks produced inside the
        // same second would otherwise be refused, which looks like a chain bug and is only a clock resolution
        // problem, so step past the tip when needed.
        const std::int64_t tip_time = chain.blocks().back().timestamp;
        const std::int64_t stamp = now > tip_time ? now : tip_time + 1;
        const std::int64_t max_timestamp = now + 120;      // a little clock slack, no more
        const std::vector<taz::Transaction> txs{tx};

        taz::ChainResult produced = chain.producers().empty()
            ? chain.append_block(txs, stamp, max_timestamp)
            : chain.append_block_signed(txs, stamp, sk, pk, max_timestamp);
        if (!produced.ok) {
            std::cerr << "taznode: the block was refused: " << produced.error << "\n";
            return 5;
        }
        std::string save_err;
        if (!taz::save_chain_json(chain, chain_file, &save_err)) {
            std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
            return 3;
        }
        std::cout << "taznode: produced block height=" << chain.height()
                  << " hash=" << taz::to_hex(chain.blocks().back().hash())
                  << " state_root=" << taz::to_hex(chain.state().state_root())
                  << (chain.producers().empty() ? " (unsigned: this chain declares no producers)" : " (signed by the producer)")
                  << "\n";
    }

    return 0;
}
