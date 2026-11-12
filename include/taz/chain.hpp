#ifndef TAZ_CHAIN_HPP
#define TAZ_CHAIN_HPP

#include "taz/block.hpp"
#include "taz/state.hpp"

namespace taz {

class Chain {
public:
    Chain();
    bool add_block(const Block& block);
    const Block* latest_block() const;
    size_t height() const;
    const State& get_state() const;

private:
    std::vector<Block> blocks_;
    State state_;
};

}

#endif
