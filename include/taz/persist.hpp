#pragma once
#include "taz/chain.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace taz {

// Saving and loading a whole chain as the same JSON file the explorer reads.
//
//  * The file holds public data only: addresses, public keys, signatures, hashes. Private keys never
//    reach this code, so they cannot be written.
//  * Loading does NOT trust the file. It re-plays the chain from genesis through Chain::accept_block, so
//    every block hash, every transaction signature, the chain ID, the protocol version, every rule and
//    every state root are checked again. A file that is wrong, cut short or tampered with is refused and
//    the Chain you passed in stays exactly as it was.
//  * Accepted formats (the "format" field): "tazvera-chain-export-v1" (plain transfers), "-v2" (safety rules),
//    "-v4" (guardian recovery with the veto key, Core v7 and later). A "-v3" file was written either by
//    Core v6 (guardian recovery WITHOUT a veto key: different signatures and state roots, refused with an
//    explanation) or by Core v7 before the label "-v4" existed (accepted as the same thing as "-v4").
//    Older layouts (Core v5 and earlier) are refused.

// v9.0: load_chain_json() is the *import* path. It is fine for tools that inspect or convert a file, and it is
// not authentication: with no pin set it will load any internally consistent chain, including one an attacker
// built. A program that starts up and joins a chain must go through load_chain_at_startup() (taz/startup.hpp),
// which refuses to load without a commitment that came from outside the file.
struct LoadOptions {
    std::uint64_t max_file_bytes = 64ull * 1024 * 1024; // refuse bigger files before reading them
    std::uint64_t max_nodes = 2'000'000;               // bound on JSON values held in memory at once
    std::uint64_t max_blocks = 200'000;                // v8.7: a file with more blocks is not one you meant to open
    // v8.3: the genesis commitment of the chain you meant to join, published out of band (release notes, a
    // signed tag, the explorer page). The file's own commitment only proves the file is internally
    // consistent: anyone can build a consistent chain with a different genesis and hand it to you, and every
    // hash inside it will agree. This pin is what answers "is this the right chain?" — leave it empty to skip.
    std::optional<Hash32> expected_genesis_commitment{};
    // v8.8: a node must never load a chain it cannot identify. When this is true a load without a pin is
    // refused instead of silently skipping the check. Off by default, so library users and the existing
    // tests are unaffected; the future node binary sets it, and a later version makes it the default.
    bool require_genesis_pin = false;
};

// Crash-safe: the data goes to a new temporary file next to the target, is flushed to disk and read back,
// and only then renamed over the target (POSIX: the directory is flushed too). A crash or a failed write
// leaves the old file untouched. Returns false and sets *error on any problem.
bool save_chain_json(const Chain& chain, const std::string& path, std::string* error = nullptr);

// Reads a chain file and verifies it from genesis. On success `out` is replaced by the loaded chain; on any
// failure `out` is not touched.
bool load_chain_json(const std::string& path, Chain& out, std::string* error = nullptr, const LoadOptions& options = {});
// Same, from text already in memory (used by tests and tools).
bool load_chain_json_text(std::string_view text, Chain& out, std::string* error = nullptr, const LoadOptions& options = {});
// v9.0: parse one block written by block_to_json(), with the same strictness the file loader applies. Used by
// block-by-block sync, so a block that arrives over the network is checked exactly like one read from disk.
bool parse_block_text(std::string_view text, Block& out, std::string* error = nullptr);

namespace testing {
// TEST ONLY: when true, save_chain_json stops after writing and flushing the temporary file and before the
// rename, and reports failure. This imitates a crash at the worst moment.
inline bool persist_stop_before_rename = false;
// Test-only process harness: pause after acquiring the writer lock until forcibly terminated.
inline bool persist_hold_after_lock = false;
inline std::string persist_lock_ready_file;
}

}  // namespace taz
