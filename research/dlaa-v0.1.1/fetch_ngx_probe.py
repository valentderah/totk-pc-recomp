"""Fetch pinned official NVIDIA SDK components into an isolated diagnostic directory."""
import hashlib,json,urllib.request
from pathlib import Path
root=Path(__file__).parent/'dependencies/nvidia-dlss'
commit='9981c6350dc58c0ce21f024fd65e31e851c31689'
request=urllib.request.Request(f'https://api.github.com/repos/NVIDIA/DLSS/git/trees/{commit}?recursive=1',headers={'User-Agent':'TotK-local-diagnostic'})
tree=json.load(urllib.request.urlopen(request));entries={e['path']:e for e in tree['tree']}
paths=['LICENSE.txt']+[e for e in entries if e.startswith('include/') and e.endswith('.h')]
paths+=['lib/Windows_x86_64/x64/nvsdk_ngx_s.lib','lib/Windows_x86_64/rel/nvngx_dlss.dll']
provenance=[]
for path in paths:
    destination=root/path;destination.parent.mkdir(parents=True,exist_ok=True)
    url=f'https://raw.githubusercontent.com/NVIDIA/DLSS/{commit}/{urllib.parse.quote(path)}'
    data=destination.read_bytes() if destination.exists() else urllib.request.urlopen(url).read()
    expected=entries[path]
    blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    if blob!=expected['sha'] or len(data)!=expected['size']:raise ValueError('Official pinned blob mismatch: '+path)
    destination.write_bytes(data)
    provenance.append(dict(path=path,url=url,bytes=len(data),git_blob_sha1=blob,sha256=hashlib.sha256(data).hexdigest()))
(root/'provenance.json').write_text(json.dumps(dict(repository='NVIDIA/DLSS',commit=commit,files=provenance),indent=2))
print('Pinned NVIDIA files verified:',len(provenance))
