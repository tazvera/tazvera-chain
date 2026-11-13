#include "taz/chain.hpp"
#include <limits>
namespace taz {
bool Chain::init_genesis(const Address& first,Amount first_balance,const Address& second,Amount second_balance){
    if(!blocks_.empty()||first==second)return false;
    State genesis_state;
    if(!genesis_state.set_genesis_balance(first,first_balance))return false;
    if(!genesis_state.set_genesis_balance(second,second_balance))return false;
    Block genesis;genesis.height=0;genesis.timestamp=0;genesis.state_root=genesis_state.state_root();
    state_=genesis_state;blocks_.push_back(genesis);return true;
}
ChainResult Chain::append_block(const std::vector<Transaction>& transactions,std::int64_t timestamp){
    if(blocks_.empty())return {false,"genesis is not initialized"};
    if(transactions.empty())return {false,"empty blocks are not allowed in this prototype"};
    if(timestamp<0||timestamp<blocks_.back().timestamp)return {false,"invalid or backwards block timestamp"};
    if(blocks_.back().height==std::numeric_limits<std::uint64_t>::max())return {false,"block height overflow"};
    State candidate=state_;
    const std::uint64_t next_height=blocks_.back().height+1;
    for(const auto& tx:transactions){const auto applied=candidate.apply(tx,next_height);if(!applied.ok)return {false,applied.error};}
    if(!candidate.supply_invariant())return {false,"supply invariant violation"};
    Block block;block.height=next_height;block.previous_hash=blocks_.back().hash();block.timestamp=timestamp;
    block.transactions=transactions;block.state_root=candidate.state_root();
    state_=std::move(candidate);blocks_.push_back(std::move(block));return {true,{}};
}
}
