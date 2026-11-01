import os
import re
import unittest
from pathlib import Path

class GpuRecorderTests(unittest.TestCase):
    def test_custom_gpu_commands_use_existing_recorder_wrappers(self):
        root=Path(__file__).resolve().parents[1]
        runtime=Path(str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'runtime'))
        header=(runtime/'vk_rec.h').read_text()
        wrapped=set(re.findall(r'#define (vkCmd\w+) rc_',header))
        source='\n'.join((root/('native/'+f)).read_text() for f in ('vk_motion_gpu.c','vk_camera_gpu.c','vk_ngx_eval.c','vk_jitter_probe.c'))
        used=set(re.findall(r'\b(vkCmd\w+)\s*\(',source))
        self.assertFalse(used-wrapped,'Unrecorded Vulkan commands: '+str(used-wrapped))
        self.assertNotRegex(source,r'\bpush\s*\(ex->cb','Push descriptors must use rec_push')
