#pragma once
#include "taz/chain.hpp"
#include "taz/persist.hpp"
#include <optional>
#include <string>

namespace taz {

// v9.0: the one place a program is allowed to load a chain from disk at startup.
//
// A chain file proves nothing about itself. Anyone can build a chain with a different genesis, hand it to you,
// and every hash inside it will agree — the file's own "genesis_commitment" field is part of the file. The only
// thing that answers "is this the chain I meant to join?" is a commitment that came from somewhere else: a
// release note, a signed tag, an operator's config file. That value is StartupConfig::trusted_genesis.
//
// The rule here is fail closed: no trusted value means no load. There is no fallback that reads the commitment
// out of the file and calls the chain authenticated, because that would be the same as trusting the file.
struct StartupConfig {
    std::string chain_file;                  // where the chain is stored
    std::optional<Hash32> trusted_genesis;   // from outside the file; never filled in from the file
};

struct StartupResult {
    bool ok{false};
    std::string error;
};

// Loads cfg.chain_file with the pin enforced. On any failure the chain in `out` is left untouched and `error`
// (when given) carries the reason. A caller that wants the plain import behaviour must call load_chain_json()
// directly — that path is documented as unauthenticated in persist.hpp.
StartupResult load_chain_at_startup(const StartupConfig& cfg, Chain& out, std::string* error = nullptr);

}  // namespace taz
