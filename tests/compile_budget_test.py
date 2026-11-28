"""tools/build_game.py compile_budget: the compile jobs fit the memory that is free, not only the RAM.

    python tests/compile_budget_test.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_game import GiB, JOB_MEM, MEM_BUDGET, compile_budget  # noqa: E402


def jobs(budget, cpus=16):
    return max(1, min(cpus, budget // JOB_MEM))   # as build_game.main


class CompileBudgetTest(unittest.TestCase):
    def test_free_memory_limits_a_pc_whose_ram_other_programs_hold(self):
        self.assertEqual(jobs(compile_budget(8 * GiB, None)), 8)          # the RAM alone: 6.4 GiB, 8 jobs
        self.assertEqual(jobs(compile_budget(8 * GiB, 3 * GiB)), 3)       # 3 GiB free: 2.4 GiB, 3 jobs
        self.assertEqual(compile_budget(8 * GiB, 3 * GiB), int(3 * GiB * 0.8))

    def test_ram_still_limits_when_more_is_reported_free(self):
        self.assertEqual(compile_budget(4 * GiB, 64 * GiB), int(4 * GiB * 0.8))

    def test_unknown_free_memory_keeps_the_old_budget(self):
        self.assertEqual(compile_budget(4 * GiB, None), int(4 * GiB * 0.8))
        self.assertEqual(compile_budget(128 * GiB, None), MEM_BUDGET)

    def test_no_free_memory_still_runs_one_job(self):
        self.assertEqual(compile_budget(4 * GiB, 0), 0)
        self.assertEqual(compile_budget(4 * GiB, -1), 0)
        self.assertEqual(jobs(compile_budget(4 * GiB, 0)), 1)


if __name__ == '__main__':
    unittest.main()
