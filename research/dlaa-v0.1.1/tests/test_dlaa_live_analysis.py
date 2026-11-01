import json,tempfile,unittest
from pathlib import Path
from analyze_dlaa_live import analyze

class LiveAnalysisTests(unittest.TestCase):
    def fixture(self,p):
        def put(name,value):(p/name).write_text(json.dumps(value))
        (p/'ngx-probe').mkdir();(p/'run.log').write_text('normal close')
        put('native-exit.json',dict(exit_code=0));put('ngx-live-shutdown.json',dict(evaluations=32,sdk_cleanup=1,gpu_completion_observed=True,continuous=True))
        put('ngx-live-frame.json',dict(index=32,frame=1031,record_result=1,presented=True,gpu_completion_observed=True,width=2560,height=1440))
        sdk={k:1 for k in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown','preset_query')};sdk.update(frames=32,creates=1,resets=1,finished=True,preset_requested=12,preset_echo=12);put('ngx-evaluate.json',sdk)
        (p/'ngx-probe/ngx-sequence-sdk.jsonl').write_text('\n'.join(json.dumps(dict(index=i,evaluate=1,creates=1,reset=int(i==1))) for i in range(1,33)))
        (p/'ngx-probe/ngx-raster-jitter-sdk.jsonl').write_text('\n'.join(json.dumps(dict(index=i,motion_vectors_jittered=True)) for i in range(1,33)))
    def test_completed_session_and_vk_error_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);self.fixture(p);analyze(p,p/'out')
            (p/'run.log').write_text('vk validation error: bad resource')
            with self.assertRaises(ValueError):analyze(p,p/'out')
    def test_failed_sdk_session_and_false_presentation_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);self.fixture(p);(p/'ngx-live-failure.json').write_text('{}')
            with self.assertRaises(ValueError):analyze(p,p/'out')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);self.fixture(p);r=json.loads((p/'ngx-live-frame.json').read_text());r['presented']=False;(p/'ngx-live-frame.json').write_text(json.dumps(r))
            with self.assertRaises(ValueError):analyze(p,p/'out')

    def test_explicit_k_required_and_sdk_echo_checked(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);self.fixture(p);r=json.loads((p/'ngx-evaluate.json').read_text());r.update(preset_requested=11,preset_echo=11);(p/'ngx-evaluate.json').write_text(json.dumps(r))
            with self.assertRaises(ValueError):analyze(p,p/'out')
            analyze(p,p/'out',preset='K');self.assertEqual(json.loads((p/'out/dlaa-live-analysis.json').read_text())['preset'],'K')
            r['preset_echo']=12;(p/'ngx-evaluate.json').write_text(json.dumps(r))
            with self.assertRaises(ValueError):analyze(p,p/'out',preset='K')
