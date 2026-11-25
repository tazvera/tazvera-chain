import unittest

from tools.consensus_network_simulator import DeterministicNetwork


class DeterministicNetworkTests(unittest.TestCase):
    def setUp(self):
        self.net = DeterministicNetwork()

    def test_delayed_message_waits_until_due(self):
        self.net.send("A", "B", "vote", now=1, delay=3)
        self.assertEqual(self.net.deliver(now=3), [])
        self.assertEqual(len(self.net.deliver(now=4)), 1)

    def test_duplicate_messages_are_delivered_as_duplicates(self):
        self.net.send("A", "B", "vote", now=0, copies=2)
        delivered = self.net.deliver(now=0)
        self.assertEqual([m.payload for m in delivered], ["vote", "vote"])

    def test_reordering_is_deterministic(self):
        self.net.send("A", "B", "first", now=0)
        self.net.send("A", "B", "second", now=0)
        self.assertEqual(
            [m.payload for m in self.net.deliver(now=0, order="lifo")],
            ["second", "first"],
        )

    def test_partition_drops_cross_partition_messages(self):
        self.net.partition([{"A", "B"}, {"C", "D"}])
        self.net.send("A", "B", "inside", now=0)
        self.net.send("A", "C", "crossing", now=0)
        delivered = self.net.deliver(now=0)
        self.assertEqual([m.payload for m in delivered], ["inside"])
        self.assertEqual(self.net.dropped, 1)

    def test_heal_allows_later_messages(self):
        self.net.partition([{"A"}, {"B"}])
        self.net.send("A", "B", "dropped", now=0)
        self.assertEqual(self.net.deliver(now=0), [])
        self.net.heal()
        self.net.send("A", "B", "after-heal", now=1)
        self.assertEqual(
            [m.payload for m in self.net.deliver(now=1)], ["after-heal"]
        )

    def test_invalid_duplicate_count_rejected(self):
        with self.assertRaises(ValueError):
            self.net.send("A", "B", "vote", now=0, copies=0)

    def test_partition_rejects_duplicate_membership(self):
        with self.assertRaises(ValueError):
            self.net.partition([{"A", "B"}, {"B", "C"}])

    def test_invalid_order_rejected(self):
        with self.assertRaises(ValueError):
            self.net.deliver(now=0, order="random")


if __name__ == "__main__":
    unittest.main()
