#include "taz/block.hpp"
#include "taz/crypto.hpp"
#include <cstring>

namespace taz {

Hash32 Block::merkle_tx_root(const std::vector<Transaction>& txs) {
  if (txs.empty()) {
    return sha256(nullptr, 0);
  }
  std::vector<Hash32> layer;
  layer.reserve(txs.size());
  for (const auto& tx : txs) layer.push_back(tx.txid());
  while (layer.size() > 1) {
    std::vector<Hash32> next;
    for (size_t i = 0; i < layer.size(); i += 2) {
      if (i + 1 == layer.size()) {
        next.push_back(layer[i]);
      } else {
        std::array<uint8_t, 64> cat{};
        std::memcpy(cat.data(), layer[i].data(), 32);
        std::memcpy(cat.data() + 32, layer[i + 1].data(), 32);
        next.push_back(sha256(cat.data(), 64));
      }
    }
    layer.swap(next);
  }
  return layer[0];
}

Hash32 Block::block_hash() const {
  std::vector<uint8_t> b;
  auto put32 = [&](uint32_t v) {
    b.push_back((v >> 24) & 0xff);
    b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 8) & 0xff);
    b.push_back(v & 0xff);
  };
  auto put64 = [&](uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
  };
  put32(header.protocol_version);
  put32(header.chain_id);
  put64(header.height);
  b.insert(b.end(), header.prev_hash.begin(), header.prev_hash.end());
  put64(header.timestamp);
  b.insert(b.end(), header.tx_root.begin(), header.tx_root.end());
  b.insert(b.end(), header.state_root.begin(), header.state_root.end());
  return sha256(b);
}

}  // namespace taz
