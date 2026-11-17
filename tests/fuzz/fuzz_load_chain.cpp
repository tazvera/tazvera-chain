// libFuzzer target: the chain-file loader is the only code here that eats attacker-shaped bytes.
// Build with -DTAZ_BUILD_FUZZERS=ON and clang (-fsanitize=fuzzer,address,undefined).
//
// Two things are checked, not just "does it crash":
//   1. a file that loads must not blow up on anything we then do with the chain;
//   2. a file that loads must conserve supply: circulating + held + burned == the genesis total.
#include "taz/persist.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > (1u << 16)) return 0;            // keep each run cheap
    taz::LoadOptions opt;
    opt.max_file_bytes = 1u << 16;
    opt.max_nodes = 20000;                                    // a fuzzer must not spend its budget in malloc
    taz::Chain chain;
    std::string err;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (!taz::load_chain_json_text(text, chain, &err, opt)) return 0;

    // It loaded: the invariants that a chain must never break.
    taz::Amount genesis_total = 0;
    for (const auto& [address, balance] : chain.genesis_balances()) {
        (void)address;
        if (balance > UINT64_MAX - genesis_total) return 0;    // impossible genesis; nothing to assert
        genesis_total += balance;
    }
    const auto& st = chain.state();
    const taz::Amount circulating = st.circulating();
    const taz::Amount held = st.held_total();
    const taz::Amount burned = st.burned_total();
    if (circulating > UINT64_MAX - held) __builtin_trap();
    if (circulating + held > UINT64_MAX - burned) __builtin_trap();
    if (circulating + held + burned != genesis_total) __builtin_trap();   // supply was created or destroyed

    // Round trip: re-exporting what we just loaded must load again to the same chain.
    const std::string again = taz::to_json(chain);
    taz::Chain copy;
    if (!taz::load_chain_json_text(again, copy, &err, opt)) __builtin_trap();
    if (copy.genesis_commitment() != chain.genesis_commitment()) __builtin_trap();
    return 0;
}
