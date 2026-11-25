"""Deterministic message-delivery fault model for consensus education.

This models transport faults only. It does not implement BFT locking,
signatures, voting power, or prove consensus safety.
"""
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class Message:
    sender: str
    recipient: str
    payload: Any
    deliver_at: int
    sequence: int


class DeterministicNetwork:
    """In-memory network with delay, duplication, reordering and partitions."""

    def __init__(self) -> None:
        self._queue: list[Message] = []
        self._sequence = 0
        self._partitions: list[frozenset[str]] | None = None
        self.dropped = 0

    def partition(self, groups: list[set[str]]) -> None:
        """Only nodes in the same group can communicate; others are isolated."""
        if not groups or any(not group for group in groups):
            raise ValueError("partition groups must be non-empty")
        flattened = [node for group in groups for node in group]
        if len(flattened) != len(set(flattened)):
            raise ValueError("a node cannot appear in multiple partition groups")
        self._partitions = [frozenset(group) for group in groups]

    def heal(self) -> None:
        self._partitions = None

    def send(self, sender: str, recipient: str, payload: Any, *,
             now: int, delay: int = 0, copies: int = 1) -> None:
        if not sender or not recipient:
            raise ValueError("sender and recipient are required")
        if now < 0 or delay < 0 or copies < 1:
            raise ValueError("invalid time, delay, or copy count")
        for _ in range(copies):
            self._queue.append(Message(sender, recipient, payload,
                                       now + delay, self._sequence))
            self._sequence += 1

    def deliver(self, *, now: int, order: str = "fifo") -> list[Message]:
        if now < 0 or order not in ("fifo", "lifo"):
            raise ValueError("invalid delivery time or order")
        due = [m for m in self._queue if m.deliver_at <= now]
        self._queue = [m for m in self._queue if m.deliver_at > now]
        due.sort(key=lambda m: m.sequence, reverse=(order == "lifo"))
        delivered = []
        for message in due:
            if self._can_communicate(message.sender, message.recipient):
                delivered.append(message)
            else:
                self.dropped += 1
        return delivered

    def pending(self) -> int:
        return len(self._queue)

    def _can_communicate(self, sender: str, recipient: str) -> bool:
        if self._partitions is None:
            return True
        return any(sender in group and recipient in group
                   for group in self._partitions)
