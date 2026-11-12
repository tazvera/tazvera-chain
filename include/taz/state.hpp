#pragma once
#include "taz/types.hpp"
#include "taz/tx.hpp"
#include <map>
#include <string>

namespace taz {

struct Account {
  Amount balance = 0;
  uint64_t nonce = 0;  // next expected nonce
};

struct ApplyResult {
  bool ok = false;
  std::string error;
  Amount burned = 0;
};

class State {
 public:
  void set_balance(const Address& a, Amount bal);
  Amount balance(const Address& a) const;
  uint64_t nonce(const Address& a) const;

  Amount circulating() const;  // sum of all account balances
  Amount burned_total() const { return burned_; }

  // Apply a signed transfer: debit amount+fee, credit amount, burn fee.
  ApplyResult apply(const Transaction& tx, uint64_t current_height);

  Hash32 state_root() const;

 private:
  std::map<Address, Account> accounts_;
  Amount burned_ = 0;
};

}  // namespace taz
