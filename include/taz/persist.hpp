#pragma once
#include "taz/chain.hpp"
#include <string>

namespace taz {

// Writes Chain::to_json explorer format to a file. Returns false on I/O error.
bool save_chain_json(const Chain& chain, const std::string& path, std::string* error = nullptr);

}  // namespace taz
