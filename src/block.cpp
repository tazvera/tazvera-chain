#include "taz/block.hpp"
#include "taz/crypto.hpp"
#include <limits>
#include <stdexcept>
namespace taz {
namespace {
void put_u32(std::vector<std::uint8_t>& out,std::uint32_t v){for(int i=3;i>=0;--i)out.push_back(static_cast<std::uint8_t>((v>>(i*8))&0xffU));}
void put_u64(std::vector<std::uint8_t>& out,std::uint64_t v){for(int i=7;i>=0;--i)out.push_back(static_cast<std::uint8_t>((v>>(i*8))&0xffU));}
void put_string(std::vector<std::uint8_t>& out,const std::string& s){if(s.size()>std::numeric_limits<std::uint32_t>::max())throw std::length_error("chain ID too long");put_u32(out,static_cast<std::uint32_t>(s.size()));out.insert(out.end(),s.begin(),s.end());}
}
Hash32 Block::hash() const {
    std::vector<std::uint8_t> out; constexpr std::string_view domain="TAZVERA-BLOCK-V1";
    out.insert(out.end(),domain.begin(),domain.end());put_u32(out,protocol_version);put_string(out,chain_id);put_u64(out,height);
    out.insert(out.end(),previous_hash.begin(),previous_hash.end());put_u64(out,static_cast<std::uint64_t>(timestamp));
    put_u64(out,static_cast<std::uint64_t>(transactions.size()));
    for(const auto& tx:transactions){const auto id=tx.txid();out.insert(out.end(),id.begin(),id.end());}
    out.insert(out.end(),state_root.begin(),state_root.end());return sha256(out);
}
}
