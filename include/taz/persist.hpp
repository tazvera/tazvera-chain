#pragma once
#include "taz/chain.hpp"
#include <string>

namespace taz {

// Explorer-compatible chain file (see export::to_json). Numbers are decimal strings;
// byte arrays are lowercase hex strings. Private keys are never written.

// Atomic save: write path+".tmp" then rename. No secrets in the file.
bool save_chain_json(const Chain& chain, const std::string& path, std::string* error = nullptr);

// Strict load: parse JSON, require known format + matching chain_id/protocol,
// rebuild via genesis + accept_block (signatures, roots, hashes verified by Chain).
// On failure `out` is left empty / unspecified; returns false and sets *error.
// Rejects trailing garbage, truncated files, wrong version, tampered hashes/roots.
bool load_chain_json(const std::string& path, Chain& out, std::string* error = nullptr);

// Same as load_chain_json but from an in-memory buffer (for tests).
bool load_chain_json_string(const std::string& json, Chain& out, std::string* error = nullptr);

}  // namespace taz
