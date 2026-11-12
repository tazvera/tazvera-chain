#include "taz/chain.hpp"

namespace taz {

Chain::Chain() {}

bool Chain::add_block(const Block& block) {
    if (block.height != blocks_.size()) return false;
    if (block.height > 0) {
        Hash32 prev = blocks_.back().hash();
        if (block.prev_hash != prev) return false;
    }
    for (const auto& tx : block.transactions) {
        ApplyResult res = state_.apply(tx, block.height);
        if (!res.ok) return false;
    }
    blocks_.push_back(block);
    return true;
}

const Block* Chain::latest_block() const {
    if (blocks_.empty()) return nullptr;
    return &blocks_.back();
}

size_t Chain::height() const { return blocks_.size(); }
const State& Chain::get_state() const { return state_; }

}
