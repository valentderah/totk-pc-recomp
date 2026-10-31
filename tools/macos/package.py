"""Package a native local app; player data is never part of a public release."""
import json
import plistlib
import shutil
from pathlib import Path

from macos.build import ROOT, INTEGRATION, BUILD, run, digest, library_prefix


def copy(source, destination):
    source, destination = Path(source), Path(destination)
    if not source.exists():
        raise RuntimeError(f'Missing application input: {source}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, destination)
    elif source.stat().st_size >= 64 * (1 << 20):
        # clonefile through cp keeps local multi-GB game copies cheap on APFS.
        import subprocess
        if subprocess.run(['cp', '-c', str(source), str(destination)], capture_output=True).returncode:
            shutil.copy2(source, destination)
    else:
        # Shader libraries are small and numerous; avoid a process per file.
        shutil.copy2(source, destination)


def package(tree, out, version, probes):
    app = out / ('TotK Recomp Diagnostics.app' if probes else 'TotK Recomp.app')
    stage = app.with_suffix('.app.partial')
    if stage.exists():
        shutil.rmtree(stage)
    macos = stage / 'Contents/MacOS'
    resources = stage / 'Contents/Resources'
    root = resources / 'totk'
    macos.mkdir(parents=True)
    root.mkdir(parents=True)
    copy(out / 'totk-native', macos / 'totk-native')
    copy(out / 'build-info.json', root / 'build-info.json')
    copy(ROOT / 'THIRD_PARTY_NOTICES.md', resources / 'THIRD_PARTY_NOTICES.md')
    copy(tree.parent.parent / 'LICENSE', resources / 'MacNativeGames-LICENSE')
    copy(tree / 'third_party/d2r_gfx/LICENSE', resources / 'd2r-LICENSE')
    copy(tree / 'third_party/d2r_gfx/LICENSE.deko3d', resources / 'deko3d-LICENSE')
    copy(tree / 'third_party/licenses', resources / 'licenses/upstream')
    copy(INTEGRATION / 'licenses/libvpx-PATENTS.txt', resources / 'licenses/libvpx-PATENTS.txt')
    for formula, archive, name in [('opus','libopus.a','COPYING'), ('libvpx','libvpx.a','LICENSE')]:
        prefix=library_prefix(formula,archive)
        copy(prefix / name, resources / 'licenses' / f'{formula}-{name}')
    if not probes:
        copy(ROOT / 'tools/vendor/ryujinx/LICENSE.txt', resources / 'licenses/Ryujinx-LICENSE.txt')
        copy(BUILD / 'shader-toolchain/SPIRV-Cross/LICENSE', resources / 'licenses/SPIRV-Cross-LICENSE')
        import subprocess
        dotnet=Path(subprocess.check_output(['brew','--prefix','dotnet'],text=True).strip())
        for name in ('LICENSE.txt','ThirdPartyNotices.txt'):
            copy(dotnet / 'share/doc/dotnet' / name, resources / 'licenses' / ('dotnet-'+name))
        for formula, archive, name in [('openssl@3','libssl.a','LICENSE.txt'), ('brotli','libbrotlicommon.a','LICENSE')]:
            prefix=library_prefix(formula,archive)
            copy(prefix / name, resources / 'licenses' / f'{formula}-{name}')
        files = [*[f'rewritten/{version}/{name}.patched.bin' for name in ('main','subsdk0','sdk')],
                 f'extracted/{version}/romfs/romfs.bin',
                 f'extracted/{version}/romfs/runtime-index.bin', f'extracted/{version}/romfs-patches',
                 f'extracted/{version}/romfs-patches-out', 'extracted/firmware/shared-fonts',
                 'build/spirv-cross/spirv-cross', 'build/shader-service/totk_xlate',
                 'build/shader-service/totk_xlate.id', 'build/pso_manifest.tsv', 'build/pso_title.tsv']
        for name in files:
            copy(tree / name, root / name)
        seed = tree / f'build/shader-seed/{version}'
        manifest = json.loads((seed / 'seed-manifest.json').read_text())
        if (manifest['version'] != version or manifest.get('shaders', 0) < 8000 or not manifest['files'] or
            (seed / 'translator.id').read_text() != (tree / 'build/shader-service/totk_xlate.id').read_text() or
            any(Path(name).name != name or digest(seed / name) != sha for name, sha in manifest['files'].items())):
            raise RuntimeError('Prepared shader seed integrity failed')
        copy(seed, root / 'build/shader-seed')
        for sha in sorted({digest(source) for source in seed.glob('*.metal')}):
            copy(tree / 'build/shader-libs' / (sha + '.metallib'), root / 'build/shader-libs' / (sha + '.metallib'))
    run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-fobjc-arc', '-O2',
         '-Wall', '-Wextra', INTEGRATION / 'launcher.m', '-framework', 'AppKit', '-framework', 'Metal',
         '-o', macos / 'TotKRecomp'])
    info = dict(CFBundleExecutable='TotKRecomp', CFBundleIdentifier='org.totkrecomp.mac.diagnostics' if probes else 'org.totkrecomp.mac',
                CFBundleName='TotK Recomp', CFBundlePackageType='APPL', CFBundleShortVersionString='0.1.0',
                CFBundleVersion='1', LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSSupportsGameMode=True, LSApplicationCategoryType='public.app-category.games',
                GCSupportsControllerUserInteraction=True)
    icon = BUILD / 'AppIcon.icns'
    if icon.is_file():
        copy(icon, resources / 'AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    (stage / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    # The bundle signature also signs its main launcher. Signing that path
    # separately would seal and hash all game resources a second time.
    run(['codesign', '--force', '--sign', '-', macos / 'totk-native'])
    run(['codesign', '--force', '--sign', '-', stage])
    run(['codesign', '--verify', '--deep', '--strict', stage])
    run([macos / 'TotKRecomp', '--check'])
    if app.exists():
        # Do not replace files underneath a running game.
        import subprocess
        commands = subprocess.check_output(['ps', '-axo', 'comm='], text=True).splitlines()
        if str(app / 'Contents/MacOS/totk-native') in (p.strip() for p in commands):
            raise RuntimeError(f'The replacement is ready at {stage}; close the game before packaging again')
        shutil.rmtree(app)
    stage.rename(app)
    return app
