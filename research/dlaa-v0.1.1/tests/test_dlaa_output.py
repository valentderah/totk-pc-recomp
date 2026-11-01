import unittest
from analyze_dlaa_output import validate_results

class DlaaOutputTests(unittest.TestCase):
    def data(self):
        sdk=dict.fromkeys(('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown'),1)
        sdk.update(available=1,finished=True,reset=True,jitter=[0,0],presented=False,temporal_quality_validated=False)
        return sdk,dict(record_result=1,gpu_completion_observed=True,sdk_cleanup=1,presented=False)
    def test_success_requires_all_sdk_results(self):
        self.assertEqual(validate_results(*self.data(),0),[])
    def test_shutdown_success_does_not_hide_release_failure(self):
        sdk,host=self.data();sdk['release']=0
        self.assertIn('SDK release not successful',validate_results(sdk,host,0))
    def test_completion_and_native_exit_required(self):
        sdk,host=self.data();host['gpu_completion_observed']=False
        self.assertIn('GPU completion missing',validate_results(sdk,host,0));self.assertIn('Native exit not zero',validate_results(sdk,host,70))
    def test_one_shot_quality_scope_must_remain_explicit(self):
        sdk,host=self.data();sdk['temporal_quality_validated']=True
        self.assertIn('Unexpected quality claim',validate_results(sdk,host,0))
