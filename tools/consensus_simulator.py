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
