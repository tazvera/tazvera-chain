#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <iostream>
int main() {
    taz::PrivKey secret{}; taz::PubKey pub{};
    if (!taz::generate_keypair(secret,pub)) { std::cerr << "Could not create demo keypair\n"; return 1; }
    const auto address=taz::address_from_pubkey(pub);
    taz::Chain chain;
    if (!chain.init_genesis(address,100000000, taz::Address{1},0)) return 1;
    std::cout << "TAZVERA local prototype ready.\n"
              << "Chain ID: " << taz::CHAIN_ID << "\n"
              << "Genesis height: " << chain.height() << "\n"
              << "Demo address: " << taz::to_hex(address) << "\n"
              << "WARNING: local test only; do not use real funds or production keys.\n";
    return 0;
}
