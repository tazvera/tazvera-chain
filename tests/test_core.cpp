#include "taz/chain.hpp"
#include "taz/crypto.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace taz;
static int failures=0;
#define CHECK(x) do { if(!(x)){ std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)
static Transaction make_tx(const PrivKey& sk,const PubKey& pk,const Address& to,Amount amount,std::uint64_t nonce) {
    Transaction tx; tx.from=address_from_pubkey(pk); tx.to=to; tx.amount=amount; tx.fee=FEE_TRANSFER_V1; tx.nonce=nonce; tx.pubkey=pk;
    const auto bytes=tx.signable_bytes(); CHECK(sign(sk,bytes,tx.signature)); return tx;
}
int main() {
    PrivKey alice_sk{},bob_sk{}; PubKey alice_pk{},bob_pk{};
    CHECK(generate_keypair(alice_sk,alice_pk)); CHECK(generate_keypair(bob_sk,bob_pk));
    const Address alice=address_from_pubkey(alice_pk), bob=address_from_pubkey(bob_pk);
    const Amount initial=100000000;
    Chain chain; CHECK(chain.init_genesis(alice,initial,bob,0)); CHECK(chain.height()==0);

    auto tx=make_tx(alice_sk,alice_pk,bob,5000000,0);
    auto msg=tx.signable_bytes(); CHECK(verify(alice_pk,msg,tx.signature));
    auto bad=tx; bad.signature[0]^=0x80; CHECK(!verify(alice_pk,bad.signable_bytes(),bad.signature));
    auto wrong_chain=tx; wrong_chain.chain_id="some-other-chain"; CHECK(!chain.append_block({wrong_chain},1).ok);
    CHECK(chain.state().balance(alice)==initial);

    auto result=chain.append_block({tx},1); CHECK(result.ok); CHECK(chain.height()==1);
    CHECK(chain.state().balance(alice)==initial-5000000-FEE_TRANSFER_V1);
    CHECK(chain.state().balance(bob)==5000000); CHECK(chain.state().burned_total()==FEE_TRANSFER_V1);
    CHECK(chain.state().circulating()+chain.state().burned_total()==initial);

    auto replay=chain.append_block({tx},2); CHECK(!replay.ok); CHECK(chain.height()==1);
    CHECK(chain.state().balance(alice)==initial-5000000-FEE_TRANSFER_V1);

    auto first=make_tx(alice_sk,alice_pk,bob,1000000,1);
    auto duplicate_nonce=make_tx(alice_sk,alice_pk,bob,1000000,1);
    const auto before=chain.state().state_root();
    auto atomic=chain.append_block({first,duplicate_nonce},2); CHECK(!atomic.ok);
    CHECK(chain.height()==1); CHECK(chain.state().state_root()==before);

    auto changed_block=chain.blocks().back(); const auto old_hash=changed_block.hash();
    changed_block.timestamp+=1; CHECK(changed_block.hash()!=old_hash);
    changed_block=chain.blocks().back(); changed_block.transactions.clear(); CHECK(changed_block.hash()!=old_hash);

    auto wrong_fee=make_tx(alice_sk,alice_pk,bob,1,1); wrong_fee.fee++;
    CHECK(!chain.append_block({wrong_fee},2).ok);
    if(failures){std::cerr<<failures<<" test(s) failed\n";return EXIT_FAILURE;}
    std::cout<<"All local core tests passed. This is not a security audit.\n"; return EXIT_SUCCESS;
}
