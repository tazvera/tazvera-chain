// taznode — the first, deliberately small, node binary.
//
// It loads the chain through load_chain_at_startup() (which refuses to load without a trusted genesis
// commitment) and prints what it loaded. Nothing listens by default: peer networking and the read-only
// loopback HTTP API are explicit opt-in modes.
#include "taz/startup.hpp"
#include "taz/crypto.hpp"
#include "taz/export.hpp"
#include "taz/persist.hpp"
#include "taz/net.hpp"
#include "taz/fork.hpp"
#include "taz/auth.hpp"
#include "taz/api.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <set>
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
    "  --new-key <path>          make a new key file and print its address, then stop (never overwrites)\n"
    "  --transfer <a>:<n>        produce one block with a transfer from the key's address to <a>\n"
    "  --register-asset <k>:<h>  produce one block that registers an asset owned by the key's address:\n"
    "                            <k> = kind 1 land, 2 vehicle, 3 invoice, 4 other; <h> = 64 hex SHA-256 of the document\n"
    "                            (prints the asset id)\n"
    "  --transfer-asset <id>:<a> produce one block that gives the asset <id> (64 hex) to the address <a> (40 hex);\n"
    "                            the key must be the asset's current owner\n"
    "  --listen <port>           wait for one peer on 127.0.0.1:<port>, sync, then stop\n"
    "  --listen-on <address>     with --listen or --serve: the IPv4 address to wait on instead of 127.0.0.1\n"
    "                            (0.0.0.0 = every interface; the connection is not encrypted)\n"
    "  --serve <port>            stay up: accept peers one at a time (see --max-peers)\n"
    "  --produce-every <seconds> with --serve (1..3600): make an empty block every N seconds, signed with --key-file\n"
    "                            (a chain that declares producers needs the key of one of them); then --max-peers\n"
    "                            does not apply unless given, and --serve-timeout still stops the node\n"
    "  --api <port>              read-only HTTP API on 127.0.0.1 (GET only; disabled by default)\n"
    "  --api-allow-origin <origin> allow exact browser Origin (repeatable; default: none)\n"
    "  --max-peers <n>           how many peers --serve accepts before stopping (default 5)\n"
    "  --serve-timeout <s>       stop waiting for peers after this many seconds (default: no limit)\n"
    "  --peer <port>             connect to 127.0.0.1:<port>, sync, then stop\n"
    "  --every <seconds>         with --peer: keep reconnecting at that interval (poll gossip)\n"
    "  --retries <n>             with --every: how many failures to survive before giving up (default 3, 0 = forever)\n"
    "  --identity <keyfile>      sign the handshake with this key, so a peer knows which key is speaking\n"
    "  --require-identity        refuse any peer that does not prove an identity (needs --identity)\n"
    "  --allow-peer <address>    allow this 40-hex peer address (repeatable; needs --require-identity)\n"
    "  --allow-peer-file <path>  read allowed 40-hex peer addresses, one per line\n"
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
// One clock, one place: the fork path and the production path must agree about "now".
std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

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
    std::string register_spec, transfer_asset_spec;
    std::string listen_port;
    std::string listen_on_address = "127.0.0.1";   // --listen-on; loopback unless the operator names another address
    bool seen_listen_on = false;
    std::string serve_port;
    std::string api_port_text;
    std::vector<std::string> api_allow_origins;
    std::string max_peers_text;
    std::string serve_timeout_text;
    std::string produce_every_text;   // --produce-every
    bool seen_produce_every = false;
    std::string every_text;
    std::string retries_text;
    std::string identity_file;
    std::vector<std::string> allow_peer_texts;
    std::vector<std::string> allow_peer_files;
    std::set<std::string> allowed_peers;
    bool allowlist_enabled = false;
    bool require_identity = false;
    std::string peer_port;
    bool seen_chain = false, seen_pin = false, seen_key = false, seen_transfer = false, seen_api = false;
    bool seen_register = false, seen_transfer_asset = false;
    std::string new_key_file;
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
        if (a == "--new-key" && i + 1 < argc) {
            if (!new_key_file.empty()) return usage_error("--new-key was given twice");
            new_key_file = argv[++i]; continue;
        }
        if (a == "--listen" && i + 1 < argc) { listen_port = argv[++i]; continue; }
        if (a == "--serve" && i + 1 < argc) { serve_port = argv[++i]; continue; }
        if (a == "--api" && i + 1 < argc) {
            if (seen_api) return usage_error("--api was given twice");
            seen_api = true; api_port_text = argv[++i]; continue;
        }
        if (a == "--api-allow-origin" && i + 1 < argc) {
            const std::string origin = argv[++i];
            if (origin == "*") return usage_error("--api-allow-origin does not accept '*'");
            const bool http = origin.rfind("http://", 0) == 0;
            const bool https = origin.rfind("https://", 0) == 0;
            const std::size_t scheme = https ? 8u : (http ? 7u : 0u);
            if (scheme == 0 || origin.size() == scheme)
                return usage_error("--api-allow-origin must be an exact http:// or https:// origin");
            for (std::size_t n = scheme; n < origin.size(); ++n) {
                const unsigned char c = static_cast<unsigned char>(origin[n]);
                if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' ||
                    c == '@' || c == '*' || c == '\\' || c == ',')
                    return usage_error("--api-allow-origin must contain only an origin, without path or credentials");
            }
            api_allow_origins.push_back(origin);
            continue;
        }
        if (a == "--max-peers" && i + 1 < argc) { max_peers_text = argv[++i]; continue; }
        if (a == "--serve-timeout" && i + 1 < argc) { serve_timeout_text = argv[++i]; continue; }
        if (a == "--every" && i + 1 < argc) { every_text = argv[++i]; continue; }
        if (a == "--retries" && i + 1 < argc) { retries_text = argv[++i]; continue; }
        if (a == "--identity" && i + 1 < argc) { identity_file = argv[++i]; continue; }
        if (a == "--allow-peer" && i + 1 < argc) { allowlist_enabled = true; allow_peer_texts.emplace_back(argv[++i]); continue; }
        if (a == "--allow-peer-file" && i + 1 < argc) { allowlist_enabled = true; allow_peer_files.emplace_back(argv[++i]); continue; }
        if (a == "--require-identity") { require_identity = true; continue; }
        if (a == "--peer" && i + 1 < argc) { peer_port = argv[++i]; continue; }
        if (a == "--transfer" && i + 1 < argc) {
            if (seen_transfer) return usage_error("--transfer was given twice");
            seen_transfer = true; transfer_spec = argv[++i]; continue;
        }
        if (a == "--produce-every" && i + 1 < argc) {
            if (seen_produce_every) return usage_error("--produce-every was given twice");
            seen_produce_every = true; produce_every_text = argv[++i]; continue;
        }
        if (a == "--listen-on" && i + 1 < argc) {
            if (seen_listen_on) return usage_error("--listen-on was given twice");
            seen_listen_on = true; listen_on_address = argv[++i]; continue;
        }
        if (a == "--register-asset" && i + 1 < argc) {
            if (seen_register) return usage_error("--register-asset was given twice");
            seen_register = true; register_spec = argv[++i]; continue;
        }
        if (a == "--transfer-asset" && i + 1 < argc) {
            if (seen_transfer_asset) return usage_error("--transfer-asset was given twice");
            seen_transfer_asset = true; transfer_asset_spec = argv[++i]; continue;
        }
        return usage_error("unknown argument \"" + a + "\"");
    }
    if (!api_allow_origins.empty() && api_port_text.empty())
        return usage_error("--api-allow-origin requires --api");
    // --transfer, --register-asset and --transfer-asset each make one block and stop: one of them per run.
    const bool production_asked = seen_transfer || seen_register || seen_transfer_asset;
    if ((seen_transfer ? 1 : 0) + (seen_register ? 1 : 0) + (seen_transfer_asset ? 1 : 0) > 1)
        return usage_error("--transfer, --register-asset and --transfer-asset each do one thing and stop; use one of them");
    if (seen_produce_every && serve_port.empty())
        return usage_error("--produce-every needs --serve: it is what a node that stays up does between peers");
    if (seen_listen_on && listen_port.empty() && serve_port.empty())
        return usage_error("--listen-on needs --listen or --serve (it is the address they wait on)");
    // Validate allowlist configuration before loading the chain or opening sockets. Empty files mean deny all.
    if (allowlist_enabled && !require_identity)
        return usage_error("--allow-peer and --allow-peer-file require --require-identity");
    auto add_allowed_peer = [&](const std::string& value, const std::string& source) -> bool {
        taz::Address address{};
        if (!parse_address(value, address)) {
            std::cerr << "taznode: invalid peer address in " << source
                      << ": expected exactly 40 hexadecimal characters\n";
            return false;
        }
        allowed_peers.insert(taz::to_hex(address));
        return true;
    };
    for (const std::string& value : allow_peer_texts)
        if (!add_allowed_peer(value, "--allow-peer")) return 2;
    for (const std::string& path : allow_peer_files) {
        std::ifstream in(path);
        if (!in) return usage_error("cannot open peer allowlist file: " + path);
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(in, line)) {
            ++line_number;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            std::size_t first = 0;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
            line.erase(0, first);
            if (line.empty()) continue;
            if (!add_allowed_peer(line, path + ":" + std::to_string(line_number))) return 2;
        }
        if (in.bad()) return usage_error("cannot read peer allowlist file: " + path);
    }

    // Making a key needs no chain, so this is handled before that check and then stops.
    if (!new_key_file.empty()) {
        if (seen_chain || !init_specs.empty() || production_asked || seen_listen_on || !peer_port.empty() || !listen_port.empty() ||
            !serve_port.empty() || !api_port_text.empty() || !api_allow_origins.empty() || seen_key || !identity_file.empty())
            return usage_error("--new-key makes a key and stops; it cannot be combined with other actions");
        if (std::ifstream(new_key_file)) return usage_error("the key file already exists; refusing to overwrite it");
        taz::PrivKey sk{};
        taz::PubKey pk{};
        if (!taz::generate_keypair(sk, pk)) {
            std::cerr << "taznode: could not generate a key\n";
            return 3;
        }
        {
            std::ofstream out(new_key_file);
            if (!out) {
                std::cerr << "taznode: cannot write the key file\n";
                return 3;
            }
            out << taz::to_hex(sk) << "\n" << taz::to_hex(pk) << "\n";
        }
        // The address is what an operator shares. The seed is NOT printed: it is already in the file, and a seed
        // on a terminal ends up in scrollback, shell history and screenshots.
        std::cout << "taznode: wrote a new key to " << new_key_file << "\n"
                  << "taznode: its address is " << taz::to_hex(taz::address_from_pubkey(pk)) << "\n"
                  << "taznode: keep that file private; anyone with it can spend from this address\n";
        return 0;
    }
    if (chain_file.empty()) return usage_error("--chain is required");

    // Creating a chain: no pin is needed, because there is nothing to trust yet. The commitment this prints is
    // what an operator hands to everyone else, and what this node will demand on every later start.
    if (!init_specs.empty()) {
        if (!api_port_text.empty()) return usage_error("--api needs an existing chain; it cannot be combined with --init-genesis");
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
        taz::Chain fresh;
        if (!fresh.init_genesis(a0, n0, a1, n1)) return usage_error("genesis could not be created (same address twice?)");
        std::string save_err;
        if (!taz::save_chain_json(fresh, chain_file, &save_err, true)) {
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

    // The HTTP API is a separate, explicitly enabled read-only mode. It never shares the chain with
    // block production or peer sync, so its concurrent readers always observe a stable Chain object.
    if (!api_port_text.empty()) {
        if (!listen_port.empty() || !serve_port.empty() || !peer_port.empty() || !transfer_spec.empty() || seen_register || seen_transfer_asset ||
            !every_text.empty() || !retries_text.empty() || !max_peers_text.empty() || !serve_timeout_text.empty())
            return usage_error("--api is a read-only service mode; do not combine it with peer sync, --transfer, --register-asset or --transfer-asset");
        unsigned int value = 0;
        const auto parsed = std::from_chars(api_port_text.data(), api_port_text.data() + api_port_text.size(), value);
        if (api_port_text.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != api_port_text.data() + api_port_text.size() || value == 0 || value > 65535)
            return usage_error("--api port must be a decimal number between 1 and 65535");
        taz::ApiServer api;
        if (!api.start(chain, static_cast<std::uint16_t>(value), api_allow_origins, &err)) {
            std::cerr << "taznode: cannot start HTTP API: " << err << "\n";
            return 3;
        }
        std::cout << "taznode: read-only HTTP API listening on 127.0.0.1:" << api.port() << std::endl;
        api.wait();
        return 0;
    }

    // ---- talk to peers ----
    // Deliberately small: one peer at a time, no threads, no peer list, no discovery. --listen serves exactly one
    // peer and stops; --serve keeps accepting, one after another, up to --max-peers. Both use the same per-peer
    // routine, so the rules cannot drift between the two modes.
    // Checked before the mode block: "--max-peers 3" on its own is a mistake whether or not a mode was given,
    // and skipping the block would have let it through as a plain load.
    if (!max_peers_text.empty() && serve_port.empty()) return usage_error("--max-peers only means something with --serve");
    if (!serve_timeout_text.empty() && serve_port.empty() && listen_port.empty())
        return usage_error("--serve-timeout only means something with --serve or --listen");
    if (!every_text.empty() && peer_port.empty()) return usage_error("--every only means something with --peer");
    if (!retries_text.empty() && every_text.empty()) return usage_error("--retries only means something with --every");
    // A node that demands an identity while offering none would be asking for what it does not give.
    if (require_identity && identity_file.empty())
        return usage_error("--require-identity needs --identity: this node would demand what it does not offer");
    if (!listen_port.empty() || !serve_port.empty() || !peer_port.empty()) {
        const int given = (!listen_port.empty() ? 1 : 0) + (!serve_port.empty() ? 1 : 0) + (!peer_port.empty() ? 1 : 0);
        if (given > 1) return usage_error("--listen, --serve and --peer are three different modes; pick one");
        if (!transfer_spec.empty())
            return usage_error("--transfer does one thing and stops; it cannot be combined with --listen, --serve or --peer");
        if (seen_register || seen_transfer_asset)
            return usage_error("--register-asset and --transfer-asset do one thing and stop; they cannot be combined with --listen, --serve or --peer");

        if (seen_listen_on) {
            if (!peer_port.empty()) return usage_error("--listen-on is the address --listen and --serve wait on; --peer connects and does not listen");
            if (!taz::is_ipv4_address(listen_on_address))
                return usage_error("--listen-on wants an IPv4 address such as 127.0.0.1 or 0.0.0.0, not \"" + listen_on_address + "\"");
        }
        auto parse_port = [](const std::string& text, std::uint16_t& out) -> bool {
            taz::Amount value = 0;
            if (!parse_amount(text, value) || value == 0 || value > 65535) return false;
            out = static_cast<std::uint16_t>(value);
            return true;
        };
        std::uint16_t port = 0;
        const std::string& port_text = !listen_port.empty() ? listen_port : (!serve_port.empty() ? serve_port : peer_port);
        if (!parse_port(port_text, port)) return usage_error("the port must be a number between 1 and 65535");

        std::uint64_t serve_timeout = 0;      // 0 = wait for ever, as a real node does
        if (!serve_timeout_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(serve_timeout_text, value) || value == 0 || value > 3600)
                return usage_error("--serve-timeout must be between 1 and 3600 seconds");
            serve_timeout = value;
        }

        std::uint64_t produce_every = 0;      // 0 = this node makes no blocks of its own
        if (seen_produce_every) {
            taz::Amount value = 0;
            if (!parse_amount(produce_every_text, value) || value == 0 || value > 3600)
                return usage_error("--produce-every must be between 1 and 3600 seconds");
            produce_every = value;
        }

        std::uint64_t max_peers = 5;
        if (!max_peers_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(max_peers_text, value) || value == 0 || value > 1000)
                return usage_error("--max-peers must be between 1 and 1000");
            max_peers = value;
        }

        // A producing node signs with the key it was given. Check that key now, before the port is opened: a node that
        // would only find out at its first block that it has no right to make one has been running for nothing.
        taz::PrivKey produce_sk{};
        taz::PubKey produce_pk{};
        if (produce_every != 0) {
            if (!key_file.empty()) {
                std::string why;
                if (!read_key_file(key_file, produce_sk, produce_pk, why)) return usage_error(why);
            }
            if (!chain.producers().empty()) {
                if (key_file.empty())
                    return usage_error("this chain declares producers, so --produce-every needs --key-file: a block must be signed by one of them");
                const taz::Address me = taz::address_from_pubkey(produce_pk);
                if (std::find(chain.producers().begin(), chain.producers().end(), me) == chain.producers().end())
                    return usage_error("the key in --key-file is not one of this chain's declared producers");
            } else if (!key_file.empty()) {
                std::cerr << "taznode: note: this chain declares no producers, so its blocks are not signed; --key-file is not used\n";
            }
        }

        taz::PrivKey identity_sk{};
        bool have_identity = false;
        if (!identity_file.empty()) {
            std::string why;
            taz::PubKey identity_pk{};
            if (!read_key_file(identity_file, identity_sk, identity_pk, why))
                return usage_error("identity key: " + why);
            taz::PubKey derived_identity_pk{};
            if (!taz::public_from_private(identity_sk, derived_identity_pk) || derived_identity_pk != identity_pk)
                return usage_error("identity key file is inconsistent: the public key does not belong to the private seed");
            have_identity = true;
        }

        std::string net_err;
        if (!taz::net_startup(&net_err)) return usage_error("network start failed: " + net_err);

        // Built per peer, not once: after adopting a peer's chain our height changes, and telling the next peer
        // the old height would be a lie it acts on.

        // Block-by-block sync: the node that is ahead sends only the blocks the other one is missing, one
        // message each. The receiver knows how many to expect from the hello heights, so no end marker is
        // needed, and every block goes through accept_block() — the same validation a block from disk gets.
        constexpr std::uint64_t kMaxBlocksPerSync = 200;   // keeps one conversation short; the next poll continues
        // announced is the height this node put in the hello, and it is what the receiver counted with. Using
        // chain.height() here instead would send one block too many whenever this node produced a block between
        // the hello and the send, and that extra block would sit in the socket and be read as the next
        // message — a protocol desync that looks like corruption.
        auto send_blocks = [&](taz::Transport& sock, std::uint64_t from_height, std::uint64_t announced) -> bool {
            if (from_height > announced) {
                std::cerr << "taznode: the peer's height (" << from_height
                          << ") is above the height we announced (" << announced << "); nothing to send\n";
                return true;
            }
            const std::uint64_t missing = announced - from_height;
            const std::uint64_t send = std::min(missing, kMaxBlocksPerSync);
            for (std::uint64_t n = 0; n < send; ++n) {
                const std::uint64_t wanted = from_height + 1 + n;
                const taz::Block* found = nullptr;
                // v9.1: blocks are stored in height order from 0, so block `wanted` sits at index `wanted`
                // (it used to walk the whole chain for every block it sent).
                if (wanted < chain.blocks().size() && chain.blocks()[static_cast<std::size_t>(wanted)].height == wanted)
                    found = &chain.blocks()[static_cast<std::size_t>(wanted)];
                if (found == nullptr) {
                    std::cerr << "taznode: our own chain is missing block " << wanted << "\n";
                    return false;
                }
                const std::string text = taz::block_to_json(*found);
                if (text.size() > taz::kMaxPayloadBytes) {
                    std::cerr << "taznode: block " << wanted << " is larger than one message\n";
                    return false;
                }
                if (!taz::send_message(sock, taz::kMsgBlocks, text, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
            }
            std::cout << "taznode: sent " << send << " block(s) from height " << (from_height + 1) << "\n";
            if (missing > send)
                std::cout << "taznode: " << (missing - send) << " block(s) left for the next sync\n";
            return true;
        };
        auto receive_blocks = [&](taz::Transport& sock, std::uint64_t expected) -> bool {
            const std::uint64_t wanted = std::min(expected, kMaxBlocksPerSync);
            // A count is not a budget: 200 messages, each allowed to stall for the receive timeout, is ten
            // minutes of a node's life handed to one peer. Time and bytes are bounded too, and the next poll
            // picks up where this one stopped.
            constexpr auto kSyncTimeBudget = std::chrono::seconds(60);
            constexpr std::size_t kSyncByteBudget = 4u * 1024u * 1024u;
            const auto started = std::chrono::steady_clock::now();
            // Passed into every read, so a peer that trickles bytes is cut off at the deadline itself rather
            // than three seconds after each of its bytes.
            const taz::Deadline deadline = started + kSyncTimeBudget;
            std::size_t bytes = 0;
            std::uint64_t added = 0;
            bool ok = true;
            for (std::uint64_t n = 0; n < wanted && ok; ++n) {
                std::uint8_t type = 0;
                std::string payload;
                if (!taz::recv_message(sock, type, payload, &net_err, deadline)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    ok = false;
                    break;
                }
                if (type != taz::kMsgBlocks) {
                    std::cerr << "taznode: the peer sent a message we did not ask for\n";
                    ok = false;
                    break;
                }
                bytes += payload.size();
                taz::Block block;
                std::string why;
                if (!taz::parse_block_text(payload, block, &why)) {
                    std::cerr << "taznode: the peer sent a block we cannot read: " << why << "\n";
                    ok = false;
                    break;
                }
                // A block with timestamp INT64_MAX is valid on its own and freezes the chain forever, because
                // every honest block after it must be "strictly greater". A peer's block is therefore only
                // accepted within the same clock slack this node allows itself when producing.
                const taz::ChainResult result = chain.accept_block(block, now_seconds() + 120);
                if (!result.ok) {
                    std::cerr << "taznode: the peer sent a block we cannot accept: " << result.error << "\n";
                    ok = false;
                    break;
                }
                ++added;
                // v9.1: the byte budget is checked AFTER the block is accepted. Checked before accepting (as in
                // v9.0), one block bigger than the budget was read, thrown away, and sent again every round
                // without ever being added: such a block could never sync.
                if (bytes > kSyncByteBudget) {
                    std::cout << "taznode: sync budget reached after " << added
                              << " block(s); the rest waits for the next round\n";
                    break;
                }
            }
            // Save whatever was accepted, even when the batch broke in the middle: a block that is in memory but
            // not on disk is a chain that disagrees with its own file after a crash.
            if (added > 0) {
                std::string save_err;
                if (!taz::save_chain_json(chain, chain_file, &save_err)) {
                    // Fail fast: this node's memory is now ahead of its own file. Continuing would mean serving
                    // answers from a chain the file does not contain, and the next restart would silently roll
                    // back work other nodes already saw.
                    std::cerr << "taznode: cannot write " << chain_file << ": " << save_err
                              << " - stopping rather than running ahead of the file\n";
                    std::exit(3);
                }
            }
            if (added > 0)
                std::cout << "taznode: accepted " << added << " block(s), height is now " << chain.height() << "\n";
            return ok;
        };

        // One conversation with one peer: handshake, decide, sync both ways. Returns false when the peer is not
        // on our chain or misbehaves; the caller decides whether that ends the program or just this peer.
        // Returns true only when the peer proved an identity (or none was required) and the exchange finished.
        // The serve loop counts that, not the bare fact that a socket opened.
        auto handle_peer = [&](taz::Transport& sock, bool we_speak_first, taz::Deadline deadline) -> bool {
            taz::Hello ours = taz::hello_for(chain);   // current height and a fresh nonce, every time
            taz::Hello theirs;
            if (require_identity) {
                // Strict mode uses the shared, tested API: it refreshes our nonce, signs the hello, checks the
                // peer's hello, and verifies the connection-bound identity proof. The CLI still performs the
                // accept-confirmation below before syncing.
                if (!taz::exchange_authenticated_hello(sock, ours, identity_sk, theirs, we_speak_first,
                                                       &net_err, deadline)) {
                    std::cerr << "taznode: authenticated handshake refused: " << net_err << "\n";
                    return false;
                }
                std::cout << "taznode: peer identity="
                          << taz::to_hex(taz::address_from_pubkey(theirs.pubkey))
                          << " (proof verified on this connection)\n";
            } else {
                // Optional-identity mode intentionally retains anonymous-peer compatibility. It still verifies
                // any identity proof that is present; it does not claim that hello-only exchange is authentication.
                if (!taz::hello_has_nonce(ours)) {
                    std::cerr << "taznode: this node could not generate a nonce; refusing to start a handshake\n";
                    return false;
                }
                // The hello's own signature is kept exactly as it was before this change: removing a check that
                // already existed is not a refactor, it is a regression with a nicer name. The auth message below is
                // the stronger, connection-bound proof; this one stays because it was already here.
                if (have_identity && !taz::sign_hello(ours, identity_sk)) {
                    std::cerr << "taznode: could not sign the handshake with the identity key\n";
                    return false;
                }
                std::string their_hello_bytes;
                if (!taz::exchange_hello(sock, ours, theirs, we_speak_first, &net_err, deadline, &their_hello_bytes)) {
                    std::cerr << "taznode: the handshake failed: " << net_err << "\n";
                    return false;
                }
                if (!taz::hello_has_nonce(theirs)) {
                    std::cerr << "taznode: the peer's hello has no nonce; refusing\n";
                    return false;
                }
                // The pre-existing hello signature check, unchanged: present and invalid is still a refusal.
                if (taz::hello_has_identity(theirs) && !taz::hello_signature_ok(theirs)) {
                    std::cerr << "taznode: the peer sent an identity signature that does not verify; refused\n";
                    return false;
                }
                const taz::HandshakeDecision decision = taz::check_peer(ours, theirs);
                if (!decision.ok) {
                    std::cerr << "taznode: peer refused: " << decision.reason << "\n";
                    return false;
                }

                // connector is A and the accepter is B, so both sides build the same bytes.
                const std::string my_hello_bytes = taz::encode_hello(ours);
                const std::string transcript =
                    taz::auth_transcript(ours, my_hello_bytes, theirs, their_hello_bytes);
                if (transcript.empty()) {
                    std::cerr << "taznode: could not build the authentication transcript; refusing\n";
                    return false;
                }
                taz::AuthMessage mine;
                if (have_identity && !taz::sign_auth(mine, identity_sk, transcript)) {
                    std::cerr << "taznode: could not sign the authentication transcript\n";
                    return false;
                }
                // Both sides send first and then read, so neither can be left waiting for a message the other will
                // only send after reading.
                if (!taz::send_message(sock, taz::kMsgAuth, taz::encode_auth(mine), &net_err, deadline)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                std::uint8_t type = 0;
                std::string payload;
                if (!taz::recv_message(sock, type, payload, &net_err, deadline)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                if (type != taz::kMsgAuth) {
                    std::cerr << "taznode: the peer did not send an identity proof\n";
                    return false;
                }
                taz::AuthMessage theirs_auth;
                std::string why;
                if (!taz::decode_auth(payload, theirs_auth, &why)) {
                    std::cerr << "taznode: the peer's identity proof cannot be read: " << why << "\n";
                    return false;
                }
                // If the peer advertised a signed hello identity, the connection proof must belong to
                // that same key. Otherwise a peer could advertise one identity and prove control of another.
                if (taz::hello_has_identity(theirs) && theirs_auth.pubkey != theirs.pubkey) {
                    std::cerr << "taznode: the connection proof key does not match the signed hello identity; refused\n";
                    return false;
                }
                bool anonymous = true;
                for (std::uint8_t b : theirs_auth.pubkey)
                    if (b != 0) anonymous = false;
                if (anonymous) {
                    std::cout << "taznode: peer is anonymous (no identity proof)\n";
                } else {
                    if (!taz::auth_ok(theirs_auth, transcript)) {
                        std::cerr << "taznode: the peer's identity proof does not verify; refused\n";
                        return false;
                    }
                    std::cout << "taznode: peer identity=" << taz::to_hex(taz::address_from_pubkey(theirs_auth.pubkey))
                              << " (proof verified on this connection)\n";
                }
            }

            // Allowlist checks happen only after the connection-bound identity proof has been verified.
            if (allowlist_enabled) {
                const std::string peer_address = taz::to_hex(taz::address_from_pubkey(theirs.pubkey));
                if (allowed_peers.find(peer_address) == allowed_peers.end()) {
                    const std::string reason = "peer address " + peer_address + " is not in the peer allowlist";
                    std::cerr << "taznode: peer refused: " << reason << "\n";
                    if (!we_speak_first) {
                        (void)taz::send_message(sock, taz::kMsgNoBlock, reason, &net_err, deadline);
                    } else {
                        std::uint8_t refusal_type = 0;
                        std::string refusal_payload;
                        if (taz::recv_message(sock, refusal_type, refusal_payload, &net_err, deadline) &&
                            refusal_type == taz::kMsgNoBlock)
                            std::cerr << "taznode: remote peer refused: " << refusal_payload << "\n";
                    }
                    return false;
                }
            }

            // A refusal must be visible to the other side. Without this the connecting node had already read its
            // peer's auth message, saw nothing wrong, and exited 0 while the server had refused it.
            if (!we_speak_first) {
                if (!taz::send_message(sock, taz::kMsgAccept, {}, &net_err, deadline)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
            } else {
                std::uint8_t accept_type = 0;
                std::string accept_payload;
                if (!taz::recv_message(sock, accept_type, accept_payload, &net_err, deadline)) {
                    std::cerr << "taznode: the peer did not confirm the connection: " << net_err << "\n";
                    return false;
                }
                if (accept_type != taz::kMsgAccept) {
                    if (accept_type == taz::kMsgNoBlock && !accept_payload.empty())
                        std::cerr << "taznode: the peer refused the connection: " << accept_payload << "\n";
                    else
                        std::cerr << "taznode: the peer did not confirm the connection\n";
                    return false;
                }
            }
            std::cout << "taznode: peer accepted chain_id=" << theirs.chain_id
                      << " protocol=" << theirs.protocol_version
                      << " height=" << theirs.best_height
                      << " genesis=" << taz::to_hex(theirs.genesis_commitment) << "\n";
            // Both sides can see the same two hellos, so this decision is symmetric: when the height and the
            // state root agree there is nothing to send in either direction, and re-sending the whole chain
            // every few seconds would be pure waste.
            // Height plus the tip hash, not the state root: two different block histories can end at the same
            // state, and skipping the exchange would leave them silently on different chains.
            const taz::Hash32 our_tip = chain.blocks().empty() ? taz::Hash32{} : chain.blocks().back().hash();
            if (theirs.best_height == chain.height() && theirs.tip_hash == our_tip) {
                std::cout << "taznode: already in sync at height " << chain.height() << "; nothing to send\n";
                return true;
            }
            // The hello height is not a per-connection work budget. send_blocks/receive_blocks already cap
            // each exchange at kMaxBlocksPerSync (200 blocks, plus time and byte budgets). Refusing a peer
            // merely because its total height is far ahead permanently strands new or long-offline nodes.
            // Let one bounded batch through; --every reconnects and continues from the newly saved height.
            // Both sides see the same two hellos, so both compute the same direction: the one that is ahead
            // sends, the one that is behind receives. Nobody waits for data the other decided not to send.
            if (chain.height() > theirs.best_height)
                return send_blocks(sock, theirs.best_height, ours.best_height);
            if (theirs.best_height > chain.height())
                return receive_blocks(sock, theirs.best_height - chain.height());
            // Same height, same genesis, different tip: a real fork. Both sides run the same four steps in the
            // same order, so neither can be left waiting: ask, read the peer's ask, answer with our tip, read
            // their tip. Then the rule in fork.cpp decides, and truncate_to() makes the decision real.
            if (chain.height() == 0) {
                // Two chains at height 0 with the same genesis commitment are the same chain; anything else here
                // would be a bug, and stepping back from 0 would underflow.
                std::cerr << "taznode: the peer disagrees at genesis; refusing to continue\n";
                return false;
            }
            {
                const std::string want = std::to_string(chain.height());
                if (!taz::send_message(sock, taz::kMsgWant, want, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                std::uint8_t type = 0;
                std::string payload;
                if (!taz::recv_message(sock, type, payload, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                if (type != taz::kMsgWant) {
                    std::cerr << "taznode: expected the peer to ask for our tip, got message type "
                              << static_cast<int>(type) << "\n";
                    return false;
                }
                if (!taz::send_message(sock, taz::kMsgBlocks, taz::block_to_json(chain.blocks().back()), &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                if (!taz::recv_message(sock, type, payload, &net_err)) {
                    std::cerr << "taznode: " << net_err << "\n";
                    return false;
                }
                if (type == taz::kMsgNoBlock) {
                    std::cerr << "taznode: the peer has no block at height " << chain.height()
                              << "; keeping our chain\n";
                    return true;
                }
                if (type != taz::kMsgBlocks) {
                    std::cerr << "taznode: expected the peer's tip block, got message type "
                              << static_cast<int>(type) << "\n";
                    return false;
                }
                taz::Block theirs_tip;
                std::string why;
                if (!taz::parse_block_text(payload, theirs_tip, &why)) {
                    std::cerr << "taznode: the peer's tip block cannot be read: " << why << "\n";
                    return false;
                }
                // Build their chain from the block before the fork, using our own history: at equal height the
                // only honest candidate we can construct is "our chain minus its tip, plus their tip".
                taz::Chain candidate = chain;
                const taz::ChainResult cut = candidate.truncate_to(chain.height() - 1);
                if (!cut.ok) {
                    std::cerr << "taznode: cannot step back to compare chains: " << cut.error << "\n";
                    return false;
                }
                const taz::ChainResult took = candidate.accept_block(theirs_tip, now_seconds() + 120);
                if (!took.ok) {
                    // This peer's tip does not attach to our local history. A deeper fork needs a
                    // common-ancestor reorganization, which this node does not implement yet. Refuse
                    // the sync explicitly: reporting success here would leave peers on divergent chains.
                    std::cerr << "taznode: refusing unrepaired deep fork: cannot attach peer tip to our history: "
                              << took.error << "\n";
                    return false;
                }
                const taz::ForkDecision fork = taz::choose_chain(chain, candidate);
                if (fork.choice != taz::ForkChoice::TakeTheirs) {
                    std::cout << "taznode: fork at height " << chain.height() << " resolved: " << fork.reason
                              << ", keeping our chain\n";
                    return true;
                }
                std::string save_err;
                if (!taz::save_chain_json(candidate, chain_file, &save_err)) {
                    std::cerr << "taznode: cannot write " << chain_file << ": " << save_err
                              << " - stopping rather than running ahead of the file\n";
                    std::exit(3);
                }
                chain = std::move(candidate);
                std::cout << "taznode: fork at height " << chain.height() << " resolved: " << fork.reason
                          << ", took the peer's chain (tip " << taz::to_hex(chain.blocks().back().hash()) << ")\n";
                return true;
            }
        };

        std::uint64_t every = 0;
        if (!every_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(every_text, value) || value == 0 || value > 3600)
                return usage_error("--every must be a number of seconds between 1 and 3600");
            every = value;
        }
        std::uint64_t retries = 3;      // 0 means "keep trying"
        if (!retries_text.empty()) {
            taz::Amount value = 0;
            if (!parse_amount(retries_text, value) || value > 1000)
                return usage_error("--retries must be a number between 0 and 1000");
            retries = value;
        }

        bool all_ok = true;
        if (!peer_port.empty()) {
            // Poll gossip: without threads this node cannot be pushed to, so it pulls. Connect, sync, close,
            // wait, repeat. It is not gossip in the academic sense and it does not scale, but it means a new
            // block reaches the other node without anyone typing a command — which is the point.
            std::uint64_t round = 0, failures = 0;
            for (;;) {
                ++round;
                taz::Socket socket;
                bool round_ok = taz::connect_loopback(port, socket, &net_err);
                if (!round_ok) {
                    std::cerr << "taznode: " << net_err << "\n";
                } else {
                    const auto auth_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    taz::PlainTransport transport(socket);   // the bytes of this connection go straight onto the socket
                    round_ok = handle_peer(transport, true, auth_deadline);
                    socket.close();
                }
                if (round_ok) {
                    failures = 0;
                    all_ok = true;
                } else {
                    ++failures;
                    all_ok = false;
                    // A peer that is down is not a reason to exit: a node that quits when the network blinks
                    // never comes back. Give up only after the retry budget, which is finite by default so a
                    // test cannot hang.
                    if (retries != 0 && failures >= retries) {
                        std::cerr << "taznode: giving up after " << failures << " failed round(s)\n";
                        break;
                    }
                }
                if (every == 0) break;
                if (round_ok) std::cout << "taznode: round " << round << " done, our height is "
                                        << chain.height() << "; waiting " << every << "s\n";
                std::this_thread::sleep_for(std::chrono::seconds(every));
            }
        } else {
            taz::Listener listener;
            if (!taz::listen_on(listen_on_address, port, listener, &net_err)) {
                std::cerr << "taznode: " << net_err << "\n";
                taz::net_cleanup();
                return 3;
            }
            // A node that makes blocks is a node, not a one-shot server: unless --max-peers says otherwise it keeps
            // serving, and --serve-timeout (or a signal) is what stops it.
            const bool unlimited = produce_every != 0 && max_peers_text.empty();
            const std::uint64_t wanted = unlimited ? std::numeric_limits<std::uint64_t>::max() : (listen_port.empty() ? max_peers : 1);
            // Anything other than 127.x is reachable from other machines; say so, and say what is missing.
            if (listen_on_address.compare(0, 4, "127.") != 0)
                std::cerr << "taznode: warning: listening on " << listen_on_address << " makes this node reachable beyond this machine; "
                             "the connection is not encrypted, so consider --identity with --require-identity\n";
            std::cout << "taznode: listening on " << listen_on_address << ":" << listener.port << " - will serve ";
            if (unlimited) std::cout << "peers until stopped"; else std::cout << wanted << " peer" << (wanted == 1 ? "" : "s");
            if (produce_every != 0) std::cout << " and make a block every " << produce_every << "s";
            std::cout << std::endl;   // v9.1: flushed, so a script that waits for this line (the tests do) is not left waiting in a stdio buffer
            // Only a peer that proved what it had to prove and finished its exchange counts towards --max-peers.
            // A refused or timed-out connection is not a served peer: otherwise a handful of anonymous connections
            // would use up the quota and stop the node.
            std::uint64_t served = 0;
            // A node that can only be stopped by killing it leaks a process in CI and hides a hang as a timeout.
            const taz::Deadline serve_deadline =
                serve_timeout == 0 ? taz::no_deadline()
                                   : std::chrono::steady_clock::now() + std::chrono::seconds(serve_timeout);
            auto quota = [&]() { return unlimited ? std::string() : " of " + std::to_string(wanted); };
            // One thread: waiting for a peer is bounded by the next block time, so the node alternates between the two.
            // A peer exchange that runs long delays the next block; blocks are not made in a burst afterwards.
            auto next_block = std::chrono::steady_clock::now() + std::chrono::seconds(produce_every);
            while (served < wanted) {
                taz::Socket peer;
                const taz::Deadline wait_until = produce_every == 0 ? serve_deadline : std::min(serve_deadline, next_block);
                if (!taz::accept_one(listener.socket, peer, &net_err, wait_until)) {
                    // select() may return a few milliseconds before the time it was given (the wait is cut to whole
                    // milliseconds, and the Windows timer is coarser still). Compare with that slack, or a node
                    // woken a hair early would take its own timer for an error and stop.
                    const auto slack = std::chrono::milliseconds(100);
                    const auto t = std::chrono::steady_clock::now();
                    const bool out_of_time = serve_timeout != 0 && t + slack >= serve_deadline;
                    if (produce_every != 0 && !out_of_time && t + slack >= next_block) {
                        std::this_thread::sleep_until(next_block);
                        // The tip is read again every time: a peer may have brought blocks since the last tick, and the
                        // chain demands a timestamp strictly greater than the tip's.
                        const std::int64_t now = now_seconds();
                        const std::int64_t tip_time = chain.blocks().back().timestamp;
                        if (tip_time >= now + 120) {
                            std::cerr << "taznode: not making a block: the chain's tip timestamp (" << tip_time
                                      << ") is beyond this clock; waiting\n";
                        } else {
                            const std::int64_t stamp = now > tip_time ? now : tip_time + 1;
                            const std::vector<taz::Transaction> none;
                            const taz::ChainResult made = chain.producers().empty()
                                ? chain.append_block(none, stamp, now + 120)
                                : chain.append_block_signed(none, stamp, produce_sk, produce_pk, now + 120);
                            if (!made.ok) {
                                // With several producers it is not always our turn; that is a skipped tick, not a crash.
                                std::cerr << "taznode: no block this time: " << made.error << "\n";
                            } else {
                                std::string save_err;
                                if (!taz::save_chain_json(chain, chain_file, &save_err)) {
                                    std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
                                    taz::net_cleanup();
                                    return 3;
                                }
                                std::cout << "taznode: produced block height=" << chain.height()
                                          << " hash=" << taz::to_hex(chain.blocks().back().hash())
                                          << (chain.producers().empty() ? " (unsigned: this chain declares no producers)" : " (signed by the producer)")
                                          << std::endl;
                            }
                        }
                        next_block = std::chrono::steady_clock::now() + std::chrono::seconds(produce_every);
                        continue;
                    }
                    // Running out of time is not a failure of this node: it served what it could.
                    std::cout << "taznode: stopped waiting after " << serve_timeout << "s with " << served
                              << quota() << " peer(s) served" << std::endl;
                    break;
                }
                // The ten seconds start when the connection is accepted, not at each read.
                const auto auth_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                taz::PlainTransport transport(peer);   // the bytes of this connection go straight onto the socket
                if (handle_peer(transport, false, auth_deadline)) {
                    ++served;
                    std::cout << "taznode: served " << served << quota()
                              << ", our height is now " << chain.height() << std::endl;
                } else {
                    all_ok = false;
                    // Flushed: a node that writes its progress into a buffer looks dead to anything watching it.
                    std::cout << "taznode: refused one connection; still waiting for "
                              << (unlimited ? std::string("peers") : std::to_string(wanted - served) + " peer(s)") << std::endl;
                }
            }
        }
        taz::net_cleanup();
        return all_ok ? 0 : 3;
    }

    // ---- block production: one transaction, one block, then save ----
    if (!transfer_spec.empty() || seen_register || seen_transfer_asset) {
        const char* what = !transfer_spec.empty() ? "--transfer" : (seen_register ? "--register-asset" : "--transfer-asset");
        if (key_file.empty()) return usage_error(std::string(what) + " needs --key-file: a transaction must be signed by its sender");
        // Everything that can be wrong with the arguments is checked before the key is read or the chain is touched.
        taz::Address to{};
        taz::Amount amount = 0;
        std::uint8_t asset_kind = 0;
        taz::Hash32 doc_or_asset{};   // --register-asset: the document hash; --transfer-asset: the asset id
        if (!transfer_spec.empty()) {
            const std::size_t colon = transfer_spec.find(':');
            if (colon == std::string::npos) return usage_error("--transfer wants <address>:<amount>");
            if (!parse_address(transfer_spec.substr(0, colon), to)) return usage_error("the transfer address must be 40 hex characters");
            if (!parse_amount(transfer_spec.substr(colon + 1), amount)) return usage_error("the transfer amount must be a decimal number");
        } else if (seen_register) {
            const std::size_t colon = register_spec.find(':');
            if (colon == std::string::npos) return usage_error("--register-asset wants <kind>:<document hash>");
            const std::string kind_text = register_spec.substr(0, colon), doc_text = register_spec.substr(colon + 1);
            taz::Amount kind_value = 0;
            if (!parse_amount(kind_text, kind_value) || kind_value < 1 || kind_value > 4)
                return usage_error("the asset kind must be 1 (land), 2 (vehicle), 3 (invoice) or 4 (other)");
            if (!is_hex64(doc_text)) return usage_error("the document hash must be 64 hex characters");
            asset_kind = static_cast<std::uint8_t>(kind_value);
            const std::vector<std::uint8_t> raw = from_hex(doc_text);
            for (std::size_t i = 0; i < doc_or_asset.size(); ++i) doc_or_asset[i] = raw[i];
        } else {
            const std::size_t colon = transfer_asset_spec.find(':');
            if (colon == std::string::npos) return usage_error("--transfer-asset wants <asset id>:<address>");
            const std::string id_text = transfer_asset_spec.substr(0, colon);
            if (!is_hex64(id_text)) return usage_error("the asset id must be 64 hex characters");
            if (!parse_address(transfer_asset_spec.substr(colon + 1), to)) return usage_error("the new owner's address must be 40 hex characters");
            const std::vector<std::uint8_t> raw = from_hex(id_text);
            for (std::size_t i = 0; i < doc_or_asset.size(); ++i) doc_or_asset[i] = raw[i];
        }

        taz::PrivKey sk{};
        taz::PubKey pk{};
        std::string why;
        if (!read_key_file(key_file, sk, pk, why)) return usage_error(why);
        const taz::Address from = taz::address_from_pubkey(pk);

        taz::Transaction tx;
        tx.from = from;
        tx.fee = taz::FEE_TRANSFER_V1;
        tx.nonce = chain.state().nonce(from);
        tx.pubkey = pk;
        if (!transfer_spec.empty()) {
            tx.to = to;
            tx.amount = amount;
        } else if (seen_register) {
            tx.kind = taz::TxKind::RegisterAsset;
            tx.owner = from;                 // an asset can only be registered by its owner, in the owner's own name
            tx.asset_kind = asset_kind;
            tx.doc_hash = doc_or_asset;
        } else {
            tx.kind = taz::TxKind::TransferAsset;
            tx.ref = doc_or_asset;           // the asset id
            tx.to = to;
            tx.owner = from;                 // the signer says he is the current owner; the chain checks the registry
        }
        const std::vector<std::uint8_t> signable = tx.signable_bytes();
        if (!taz::sign(sk, signable, tx.signature)) return usage_error("the key cannot sign (is the key file right?)");
        // The file carries the seed and the public key side by side, so they can disagree. Signing with one and
        // claiming the other would produce a transaction that fails much later with a confusing message; catch
        // it here instead.
        if (!taz::verify(pk, signable, tx.signature))
            return usage_error("the key file is inconsistent: that public key does not belong to that seed");

        const std::int64_t now = now_seconds();
        // The chain requires a timestamp strictly greater than the previous block. Two blocks produced inside the
        // same second would otherwise be refused, which looks like a chain bug and is only a clock resolution
        // problem, so step past the tip when needed.
        const std::int64_t tip_time = chain.blocks().back().timestamp;
        if (tip_time >= now + 120) {
            std::cerr << "taznode: the chain's tip timestamp (" << tip_time
                      << ") is beyond this clock; refusing to extend a chain that cannot be extended\n";
            return 5;
        }
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
        // A block nobody can transport is a block nobody can verify: the sync path refuses anything larger than
        // one message, so producing one would fork this node off the network by itself. Refuse to make it, and
        // put the chain back the way it was — nothing has been written yet, so a reload is the honest rollback.
        if (taz::block_to_json(chain.blocks().back()).size() > taz::kMaxPayloadBytes) {
            std::cerr << "taznode: this block is larger than one sync message (" << taz::kMaxPayloadBytes
                      << " bytes), so no other node could ever receive it; not producing it\n";
            taz::Chain restored;
            std::string reload_err;
            if (taz::load_chain_json_text(taz::to_json(chain), restored, &reload_err))
                chain = std::move(restored);
            return 5;
        }
        std::string save_err;
        if (!taz::save_chain_json(chain, chain_file, &save_err)) {
            std::cerr << "taznode: cannot write " << chain_file << ": " << save_err << "\n";
            return 3;
        }
        if (seen_register)   // the id is made from chain id, owner and nonce; print it so it can be used with --transfer-asset
            std::cout << "taznode: registered asset id=" << taz::to_hex(taz::asset_id(tx.chain_id, tx.owner, tx.nonce)) << "\n";
        std::cout << "taznode: produced block height=" << chain.height()
                  << " hash=" << taz::to_hex(chain.blocks().back().hash())
                  << " state_root=" << taz::to_hex(chain.state().state_root())
                  << (chain.producers().empty() ? " (unsigned: this chain declares no producers)" : " (signed by the producer)")
                  << "\n";
    }

    return 0;
}
