#include "taz/startup.hpp"

namespace taz {

StartupResult load_chain_at_startup(const StartupConfig& cfg, Chain& out, std::string* error) {
    auto stop = [&](std::string message) {
        if (error) *error = message;
        return StartupResult{false, std::move(message)};
    };

    // Fail closed, first thing: a startup without a trusted commitment is a startup that cannot tell one chain
    // from another, so it does not happen.
    if (!cfg.trusted_genesis) return stop("refusing to start: no trusted genesis commitment is configured");
    if (cfg.chain_file.empty()) return stop("refusing to start: no chain file is configured");

    LoadOptions options;
    options.require_genesis_pin = true;                       // belt: the loader enforces it as well
    options.expected_genesis_commitment = cfg.trusted_genesis; // braces: and this is the value it must match

    std::string loader_error;
    if (!load_chain_json(cfg.chain_file, out, &loader_error, options))
        return stop("cannot load " + cfg.chain_file + ": " + loader_error);
    return StartupResult{true, {}};
}

}  // namespace taz
