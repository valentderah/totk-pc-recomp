import hashlib,json,tempfile,unittest
from pathlib import Path
from prepare_dlaa_replay import verify_manifest,tsv_text

class ReplayManifestTests(unittest.TestCase):
    def fixture(self,root):
        p=root/'sample.bin';p.write_bytes(b'original capture');item=dict(path=p.as_posix(),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        rows=[dict(frame=f,jitter=[0,.25],frame_time_ms=16.666667,inputs=[item]*4,original_output=item) for f in range(1,9)]
        (root/'manifest.json').write_text(json.dumps(dict(width=2560,height=1440,frames=rows,exact_depth=True)))
        (root/'replay.tsv').write_text(tsv_text(2560,1440,rows));return p
    def test_same_hashes_and_command_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);self.fixture(root);verify_manifest(root)
            (root/'replay.tsv').write_text('changed')
            with self.assertRaises(ValueError):verify_manifest(root)
    def test_changed_input_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=self.fixture(root);p.write_bytes(b'changed capture!')
            with self.assertRaises(ValueError):verify_manifest(root)
    def test_duplicate_path_with_conflicting_hash_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);self.fixture(root);data=json.loads((root/'manifest.json').read_text());data['frames'][0]['inputs'][1]['sha256']='0'*64
            (root/'manifest.json').write_text(json.dumps(data))
            with self.assertRaises(ValueError):verify_manifest(root)
