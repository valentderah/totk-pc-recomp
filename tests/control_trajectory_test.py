"""Interpolation checks for differently sampled actor trajectories."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compare_control_trajectories import sample, compare


def p(t, x, actor='one'):
    return dict(type='actor_position', world_us=t, position=[x, x*2, 0], actor=actor)


class Trajectories(unittest.TestCase):
    def test_interpolation_and_no_extrapolation(self):
        rows=[p(0,0),p(100000,1)]
        self.assertEqual(sample(rows,[0,100000],50000),[0.5,1,0])
        self.assertIsNone(sample(rows,[0,100000],-1))
        self.assertIsNone(sample(rows,[0,100000],100001))
        self.assertIsNone(sample([p(0,0),p(200000,2)],[0,200000],50000))
        self.assertIsNone(sample([p(0,0),p(100000,1,'two')],[0,100000],50000))

    def test_sampling_difference_does_not_create_motion_difference(self):
        a=[p(0,0),p(100000,1),p(200000,2)]
        b=[p(0,0),p(50000,.5),p(150000,1.5),p(200000,2)]
        result=compare(a,b,0,200000)
        self.assertEqual(result['max_distance_m'],0)
        self.assertEqual(result['missing_world_us'],[])


if __name__=='__main__':
    unittest.main()
