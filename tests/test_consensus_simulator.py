import unittest

from tools.consensus_simulator import Round, four_validator_demo


class ConsensusSimulatorTests(unittest.TestCase):
    def setUp(self):
        self.sim = Round({"A", "B", "C", "D"})

    def test_three_of_four_prevotes_and_precommits_commit(self):
        self.assertTrue(four_validator_demo())

    def test_two_votes_are_not_quorum(self):
        self.sim.vote("prevote", "A", "x")
        self.sim.vote("prevote", "B", "x")
        self.assertFalse(self.sim.has_prevote_quorum("x"))

    def test_two_precommits_do_not_commit(self):
        self.sim.vote("precommit", "A", "x")
        self.sim.vote("precommit", "B", "x")
        self.assertIsNone(self.sim.committed)

    def test_duplicate_vote_rejected(self):
        self.sim.vote("prevote", "A", "x")
        with self.assertRaises(ValueError):
            self.sim.vote("prevote", "A", "y")

    def test_unknown_validator_rejected(self):
        with self.assertRaises(ValueError):
            self.sim.vote("prevote", "E", "x")

    def test_invalid_phase_rejected(self):
        with self.assertRaises(ValueError):
            self.sim.vote("proposal", "A", "x")

    def test_no_committing_two_blocks_with_same_votes(self):
        for validator in ("A", "B", "C"):
            self.sim.vote("precommit", validator, "x")
        self.assertEqual(self.sim.committed, "x")
        with self.assertRaises(ValueError):
            self.sim.vote("precommit", "D", "y")

    def test_round_change_clears_votes(self):
        self.sim.vote("prevote", "A", "x")
        self.sim.next_round()
        self.assertEqual(self.sim.round, 1)
        self.assertFalse(self.sim.has_prevote_quorum("x"))


if __name__ == "__main__":
    unittest.main()
