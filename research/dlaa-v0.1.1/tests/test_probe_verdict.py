import unittest
from verify_gpu_probe import verdict

class ProbeVerdictTests(unittest.TestCase):
    def base(self):
        return dict(exit_code=0,log='vk_rec: the replay\'s Vulkan commands are made by a recorder thread\nboot: validation errors 0',
                    current=dict(eligible=25,drawn=25,history=1),valid_pixels=42,finite=True,bounds_pixels=0)

    def test_live_parallel_probe(self):
        self.assertEqual(verdict(**self.base()),[])

    def test_boot_summary_is_not_an_error(self):
        self.assertNotIn('Vulkan errors',verdict(**self.base()))

    def test_error_even_if_exit_zero(self):
        d=self.base();d['log']+='\nvk validation error: VUID broken'
        self.assertIn('Vulkan errors',verdict(**d))

    def test_empty_loading_exports_do_not_validate_bounds(self):
        d=self.base();d.update(current=dict(eligible=0,drawn=0,history=1),valid_pixels=0)
        self.assertIn('No live motion draws',verdict(**d));self.assertIn('Empty motion output',verdict(**d))

    def test_invalid_palette_must_reject_all(self):
        d=self.base();d['bounds_pixels']=1
        self.assertIn('Invalid palette accepted pixels',verdict(**d))

    def test_nonfinite_and_native_crash(self):
        d=self.base();d.update(finite=False,exit_code=3221225477)
        self.assertIn('Native exit is not zero',verdict(**d));self.assertIn('Nonfinite accepted motion',verdict(**d))
