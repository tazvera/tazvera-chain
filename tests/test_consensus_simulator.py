import unittest

from tools.consensus_simulator import (
    Round, ValidatorLifecycle, four_validator_demo,
)


class ConsensusSimulatorTests(unittest.TestCase):
    def setUp(self):
        self.sim = Round({"A", "B", "C", "D"})

    def establish_prevote_quorum(self, block="x"):
        for validator in ("A", "B", "C"):
            self.sim.vote("prevote", validator, block)

    def test_three_of_four_prevotes_and_precommits_commit(self):
        self.assertTrue(four_validator_demo())

    def test_two_votes_are_not_quorum(self):
        self.sim.vote("prevote", "A", "x")
        self.sim.vote("prevote", "B", "x")
        self.assertFalse(self.sim.has_prevote_quorum("x"))

    def test_precommit_requires_prevote_quorum(self):
        with self.assertRaises(ValueError):
            self.sim.vote("precommit", "A", "x")

    def test_two_precommits_do_not_commit(self):
        self.establish_prevote_quorum()
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

    def test_commit_freezes_height(self):
        self.establish_prevote_quorum()
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


class ValidatorLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.registry = ValidatorLifecycle(
            stakes={"A": 100, "B": 100, "C": 100, "D": 100}
        )

    def test_off_takes_effect_only_at_target_epoch(self):
        self.registry.schedule_status("D", False, effective_epoch=2)
        self.assertIn("D", self.registry.active)
        self.registry.advance_epoch(1)
        self.assertIn("D", self.registry.active)
        self.registry.advance_epoch(2)
        self.assertNotIn("D", self.registry.active)

    def test_on_takes_effect_only_at_target_epoch(self):
        self.registry.schedule_status("D", False, effective_epoch=1)
        self.registry.advance_epoch(1)
        self.assertNotIn("D", self.registry.active)
        self.registry.schedule_status("D", True, effective_epoch=3)
        self.registry.advance_epoch(2)
        self.assertNotIn("D", self.registry.active)
        self.registry.advance_epoch(3)
        self.assertIn("D", self.registry.active)

    def test_cannot_unbond_while_active(self):
        with self.assertRaises(ValueError):
            self.registry.request_unbond("A", 2, 3)

    def test_unbond_requires_delay_and_evidence_window(self):
        self.registry.schedule_status("D", False, effective_epoch=1)
        self.registry.advance_epoch(1)
        self.registry.request_unbond("D", delay_epochs=2,
                                     evidence_until_epoch=4)
        self.registry.advance_epoch(2)
        self.registry.advance_epoch(3)
        self.assertFalse(self.registry.withdrawable("D"))
        self.registry.advance_epoch(4)
        self.assertTrue(self.registry.withdrawable("D"))

    def test_unbond_cannot_be_requested_twice(self):
        self.registry.schedule_status("D", False, effective_epoch=1)
        self.registry.advance_epoch(1)
        self.registry.request_unbond("D", 2, 2)
        with self.assertRaises(ValueError):
            self.registry.request_unbond("D", 2, 2)

    def test_epoch_cannot_be_skipped_or_rewound(self):
        with self.assertRaises(ValueError):
            self.registry.advance_epoch(2)

    def test_status_change_cannot_be_replayed_for_current_epoch(self):
        with self.assertRaises(ValueError):
            self.registry.schedule_status("A", False, effective_epoch=0)

    def test_withdraw_before_unlock_is_rejected(self):
        self.registry.schedule_status("D", False, effective_epoch=1)
        self.registry.advance_epoch(1)
        self.registry.request_unbond("D", 2, 3)
        with self.assertRaises(ValueError):
            self.registry.withdraw("D")

    def test_withdraw_is_one_time_and_removes_stake(self):
        self.registry.schedule_status("D", False, effective_epoch=1)
        self.registry.advance_epoch(1)
        self.registry.request_unbond("D", 1, 2)
        self.registry.advance_epoch(2)
        self.assertEqual(self.registry.withdraw("D"), 100)
        self.assertNotIn("D", self.registry.stakes)
        with self.assertRaises(ValueError):
            self.registry.withdraw("D")
