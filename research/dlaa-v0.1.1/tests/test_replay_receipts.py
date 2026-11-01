import hashlib,json,tempfile,unittest
from pathlib import Path
from compare_dlaa_presets import validate_receipts

class ReplayReceiptTests(unittest.TestCase):
    def fixture(self,p):
        inputs=p/'inputs';inputs.mkdir();(inputs/'manifest.json').write_text('manifest');(inputs/'replay.tsv').write_text('commands');roots=[]
        for preset in (0,11,12):
            root=p/str(preset);root.mkdir();roots.append(root)
            r=dict(manifest_json_sha256=hashlib.sha256(b'manifest').hexdigest(),manifest_tsv_sha256=hashlib.sha256(b'commands').hexdigest(),bridge_sha256='A'*64,runtime_sha256='B'*64,preset_requested=preset,verified_before_and_after_gpu_completion=True,runtime_signed_nvidia=True)
            (root/'input-receipt.json').write_text(json.dumps(r))
        return inputs,roots
    def test_receipts_match_inputs_and_each_other(self):
        with tempfile.TemporaryDirectory() as d:
            inputs,roots=self.fixture(Path(d));validate_receipts(inputs,roots)
            p=roots[2]/'input-receipt.json';r=json.loads(p.read_text());r['manifest_tsv_sha256']='0'*64;p.write_text(json.dumps(r))
            with self.assertRaises(ValueError):validate_receipts(inputs,roots)
    def test_changed_runtime_or_unconfirmed_provenance_rejected(self):
        for key,value in [('runtime_sha256','C'*64),('bridge_sha256','D'*64),('verified_before_and_after_gpu_completion',False)]:
            with tempfile.TemporaryDirectory() as d:
                inputs,roots=self.fixture(Path(d));p=roots[1]/'input-receipt.json';r=json.loads(p.read_text());r[key]=value;p.write_text(json.dumps(r))
                with self.assertRaises(ValueError):validate_receipts(inputs,roots)
