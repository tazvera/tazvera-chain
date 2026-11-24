#pragma once
// Protocol 4: a transaction is signed for ONE network. Most tests build a chain, then sign transactions with small
// helpers that do not know which chain they are for; this holds "the network of the chain this test is working on".
// Call taztest::use(chain) (or assign g_net) right after init_genesis; the helpers copy g_net into every transaction
// before signing it. Tests that need a SECOND network, or a deliberately wrong one, set tx.network_id themselves.
#include "taz/chain.hpp"
namespace taztest {
inline taz::Hash32 g_net{};
inline void use(const taz::Chain& c) { g_net = c.network_id(); }
}
