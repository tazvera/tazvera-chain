#include "taz/persist.hpp"
#include "taz/export.hpp"
#include <cstdio>
#include <fstream>

namespace taz {

bool save_chain_json(const Chain& chain, const std::string& path, std::string* error) {
    try {
        const std::string body = to_json(chain);
        // Write to a temporary file first and rename it, so a crash never leaves a half-written chain file.
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { if (error) *error = "could not open file for write"; return false; }
            out << body;
            out.flush();
            if (!out) { std::remove(tmp.c_str()); if (error) *error = "write failed"; return false; }
        }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            if (error) *error = "could not replace the chain file";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace taz
