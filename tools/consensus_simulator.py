"""Deterministic educational model of a 4-validator BFT round.

This is a simulator, not production consensus or cryptographic code.
It models prevote quorum, precommit quorum, and commit.
"""
from dataclasses import dataclass, field
from typing import Dict, Optional, Set

Validator = str
Block = str


@dataclass
class Round:
    validators: Set[Validator]
    height: int = 1
    round: int = 0
    prevotes: Dict[Block, Set[Validator]] = field(default_factory=dict)
    precommits: Dict[Block, Set[Validator]] = field(default_factory=dict)
    committed: Optional[Block] = None

    def __post_init__(self) -> None:
        if len(self.validators) < 4:
            raise ValueError("BFT simulator requires at least 4 validators")
        self.quorum = (2 * len(self.validators)) // 3 + 1

    def vote(self, phase: str, validator: Validator, block: Block) -> bool:
        """Record a vote; enforce validator uniqueness and prevote-before-precommit."""
        if validator not in self.validators:
            raise ValueError("unknown validator")
        if self.committed is not None:
            raise ValueError("height already committed")
        if phase not in ("prevote", "precommit"):
            raise ValueError("phase must be prevote or precommit")
        if phase == "precommit" and not self.has_prevote_quorum(block):
            raise ValueError("precommit requires prevote quorum for this block")
        target = self.prevotes if phase == "prevote" else self.precommits
        if any(validator in voters for voters in target.values()):
            raise ValueError("validator already voted in this phase")
        target.setdefault(block, set()).add(validator)
        if phase == "precommit" and len(target[block]) >= self.quorum:
            self.committed = block
        return self.committed == block

    def has_prevote_quorum(self, block: Block) -> bool:
        return len(self.prevotes.get(block, set())) >= self.quorum

    def has_commit_quorum(self, block: Block) -> bool:
        return len(self.precommits.get(block, set())) >= self.quorum

    def next_round(self) -> None:
        if self.committed is not None:
            raise ValueError("cannot advance a committed height")
        self.round += 1
        self.prevotes.clear()
        self.precommits.clear()


def four_validator_demo() -> bool:
    sim = Round({"A", "B", "C", "D"})
    for validator in ("A", "B", "C"):
        sim.vote("prevote", validator, "block-1")
    if not sim.has_prevote_quorum("block-1"):
        return False
    for validator in ("A", "B", "C"):
        sim.vote("precommit", validator, "block-1")
    return sim.committed == "block-1"


if __name__ == "__main__":
    print("COMMITTED" if four_validator_demo() else "NOT COMMITTED")



@dataclass
class ValidatorLifecycle:
    """Educational model for epoch-bound ON/OFF and delayed stake unlock.

    This is not production staking code. It intentionally has no money,
    signatures, slashing, persistence, or network integration.
    """
    stakes: Dict[Validator, int]
    active: Set[Validator] = field(default_factory=set)
    epoch: int = 0
    status_changes: Dict[Validator, tuple] = field(default_factory=dict)
    unbonding: Dict[Validator, tuple] = field(default_factory=dict)
    withdrawn: Set[Validator] = field(default_factory=set)

    def __post_init__(self) -> None:
        if not self.stakes or any(amount <= 0 for amount in self.stakes.values()):
            raise ValueError("every validator must have positive stake")
        if not self.active:
            self.active = set(self.stakes)
        if not self.active.issubset(self.stakes):
            raise ValueError("active set contains unknown validator")

    def schedule_status(self, validator: Validator, enabled: bool,
                        effective_epoch: int) -> None:
        if validator not in self.stakes:
            raise ValueError("unknown validator")
        if validator in self.unbonding:
            raise ValueError("cannot change status while unbonding")
        if effective_epoch <= self.epoch:
            raise ValueError("status change must target a future epoch")
        if validator in self.status_changes:
            raise ValueError("validator already has a pending status change")
        self.status_changes[validator] = (enabled, effective_epoch)

    def advance_epoch(self, new_epoch: int) -> None:
        if new_epoch != self.epoch + 1:
            raise ValueError("epochs must advance one at a time")
        self.epoch = new_epoch
        for validator, (enabled, target_epoch) in list(self.status_changes.items()):
            if target_epoch == self.epoch:
                if enabled:
                    self.active.add(validator)
                else:
                    self.active.discard(validator)
                del self.status_changes[validator]

    def request_unbond(self, validator: Validator, delay_epochs: int,
                       evidence_until_epoch: int) -> None:
        if validator not in self.stakes:
            raise ValueError("unknown validator")
        if validator in self.withdrawn:
            raise ValueError("stake already withdrawn")
        if validator in self.active:
            raise ValueError("validator must be OFF before unbonding")
        if validator in self.unbonding:
            raise ValueError("unbonding already requested")
        if validator in self.status_changes:
            raise ValueError("pending status change must settle before unbonding")
        if delay_epochs <= 0 or evidence_until_epoch < self.epoch:
            raise ValueError("invalid unbonding or evidence window")
        self.unbonding[validator] = (
            self.epoch + delay_epochs, evidence_until_epoch
        )

    def withdrawable(self, validator: Validator) -> bool:
        request = self.unbonding.get(validator)
        if request is None:
            return False
        unlock_epoch, evidence_until_epoch = request
        return self.epoch >= unlock_epoch and self.epoch >= evidence_until_epoch

    def withdraw(self, validator: Validator) -> int:
        """Finalize a one-time withdrawal after all modeled delays."""
        if not self.withdrawable(validator):
            raise ValueError("stake is not withdrawable")
        amount = self.stakes.pop(validator)
        del self.unbonding[validator]
        self.withdrawn.add(validator)
        return amount


@dataclass
class WeightedRound:
    """Educational quorum model using validator voting power.

    This deliberately does not implement proposal selection, locks, signatures,
    persistence, or network consensus.
    """
    stakes: Dict[Validator, int]
    height: int = 1
    round: int = 0
    prevotes: Dict[Block, Set[Validator]] = field(default_factory=dict)
    precommits: Dict[Block, Set[Validator]] = field(default_factory=dict)
    committed: Optional[Block] = None

    def __post_init__(self) -> None:
        if len(self.stakes) < 4:
            raise ValueError("BFT simulator requires at least 4 validators")
        if any(not isinstance(power, int) or isinstance(power, bool) or power <= 0
               for power in self.stakes.values()):
            raise ValueError("validator voting power must be a positive integer")
        self.total_power = sum(self.stakes.values())
        self.quorum = (2 * self.total_power) // 3 + 1

    def _power(self, votes: Dict[Block, Set[Validator]], block: Block) -> int:
        return sum(self.stakes[v] for v in votes.get(block, set()))

    def has_prevote_quorum(self, block: Block) -> bool:
        return self._power(self.prevotes, block) >= self.quorum

    def has_commit_quorum(self, block: Block) -> bool:
        return self._power(self.precommits, block) >= self.quorum

    def vote(self, phase: str, validator: Validator, block: Block) -> bool:
        if validator not in self.stakes:
            raise ValueError("unknown validator")
        if self.committed is not None:
            raise ValueError("height already committed")
        if phase not in ("prevote", "precommit"):
            raise ValueError("phase must be prevote or precommit")
        if not isinstance(block, str) or not block:
            raise ValueError("block identifier is required")
        if phase == "precommit" and not self.has_prevote_quorum(block):
            raise ValueError("precommit requires prevote quorum for this block")
        target = self.prevotes if phase == "prevote" else self.precommits
        if any(validator in voters for voters in target.values()):
            raise ValueError("validator already voted in this phase")
        target.setdefault(block, set()).add(validator)
        if phase == "precommit" and self.has_commit_quorum(block):
            self.committed = block
        return self.committed == block
