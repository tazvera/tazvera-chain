#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <iostream>
int main(){
    taz::PrivKey secret{};taz::PubKey pub{};
    if(!taz::generate_keypair(secret,pub)){std::cerr<<"Could not create demo keypair\n";return 1;}
    const auto address=taz::address_from_pubkey(pub);
    taz::Chain chain;
    if(!chain.init_genesis(address,100000000,taz::Address{1},0)){std::cerr<<"Genesis initialization failed\n";return 1;}
    std::cout<<"TAZVERA local core prototype ready.\nChain ID: "<<taz::CHAIN_ID
             <<"\nGenesis height: "<<chain.height()<<"\nDemo address: "<<taz::to_hex(address)
             <<"\nWARNING: local test only; do not use real funds or production keys.\n";
    return 0;
}
