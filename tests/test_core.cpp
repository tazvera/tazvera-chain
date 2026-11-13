#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace taz;
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static Transaction make_tx(const PrivKey& sk,const PubKey& pk,const Address& to,Amount amount,std::uint64_t nonce){
    Transaction tx;tx.from=address_from_pubkey(pk);tx.to=to;tx.amount=amount;tx.fee=FEE_TRANSFER_V1;tx.nonce=nonce;tx.pubkey=pk;
    const auto bytes=tx.signable_bytes();CHECK(sign(sk,bytes,tx.signature));return tx;
}
int main(){
    PrivKey alice_sk{},bob_sk{};PubKey alice_pk{},bob_pk{};
    CHECK(generate_keypair(alice_sk,alice_pk));CHECK(generate_keypair(bob_sk,bob_pk));
    const Address alice=address_from_pubkey(alice_pk),bob=address_from_pubkey(bob_pk);
    const Amount initial=100000000;
    Chain chain;CHECK(chain.init_genesis(alice,initial,bob,0));CHECK(chain.height()==0);
    CHECK(!chain.init_genesis(alice,initial,bob,0));

    auto tx=make_tx(alice_sk,alice_pk,bob,5000000,0);
    auto msg=tx.signable_bytes();CHECK(verify(alice_pk,msg,tx.signature));
    auto bad=tx;bad.signature[0]^=0x80;CHECK(!verify(alice_pk,bad.signable_bytes(),bad.signature));
    auto wrong_chain=tx;wrong_chain.chain_id="wrong-chain";CHECK(!chain.append_block({wrong_chain},1).ok);
    auto wrong_fee=tx;wrong_fee.fee++;CHECK(!chain.append_block({wrong_fee},1).ok);
    auto wrong_sender=tx;wrong_sender.from[0]^=1;CHECK(!chain.append_block({wrong_sender},1).ok);
    CHECK(chain.state().balance(alice)==initial);

    CHECK(!chain.append_block({},1).ok); // empty block
    auto result=chain.append_block({tx},1);CHECK(result.ok);CHECK(chain.height()==1);
    CHECK(chain.state().balance(alice)==initial-5000000-FEE_TRANSFER_V1);
    CHECK(chain.state().balance(bob)==5000000);CHECK(chain.state().burned_total()==FEE_TRANSFER_V1);
    CHECK(chain.state().circulating()+chain.state().burned_total()==initial);CHECK(chain.state().supply_invariant());

    auto replay=chain.append_block({tx},2);CHECK(!replay.ok);CHECK(chain.height()==1);
    CHECK(chain.state().balance(alice)==initial-5000000-FEE_TRANSFER_V1);
    auto first=make_tx(alice_sk,alice_pk,bob,1000000,1);
    auto duplicate=make_tx(alice_sk,alice_pk,bob,1000000,1);
    const auto before=chain.state().state_root();auto atomic=chain.append_block({first,duplicate},2);
    CHECK(!atomic.ok);CHECK(chain.height()==1);CHECK(chain.state().state_root()==before);
    auto expired=make_tx(alice_sk,alice_pk,bob,1,1);expired.expiry_height=1;
    auto expired_bytes=expired.signable_bytes();CHECK(sign(alice_sk,expired_bytes,expired.signature));CHECK(!chain.append_block({expired},2).ok);
    CHECK(!chain.append_block({first},0).ok); // timestamp cannot go backwards

    auto changed=chain.blocks().back();const auto old_hash=changed.hash();changed.timestamp+=1;CHECK(changed.hash()!=old_hash);
    changed=chain.blocks().back();changed.transactions.clear();CHECK(changed.hash()!=old_hash);
    changed=chain.blocks().back();changed.state_root[0]^=1;CHECK(changed.hash()!=old_hash);

    State supply;Address a{},b{};a[0]=1;b[0]=2;
    CHECK(supply.set_genesis_balance(a,MAX_SUPPLY));CHECK(!supply.set_genesis_balance(b,1));
    State overflow;CHECK(overflow.set_genesis_balance(a,MAX_SUPPLY+1)==false);

    if(failures){std::cerr<<failures<<" test(s) failed\n";return EXIT_FAILURE;}
    std::cout<<"All local core tests passed. This is not a security audit.\n";return EXIT_SUCCESS;
}
