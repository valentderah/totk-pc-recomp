"""Extract the game named in config.local.toml into build/ and verify it against known hashes.

Steps, each skipped when its verified output already exists:

1. ExeFS (code modules) of the base game and of the update.
2. Base RomFS, verified through all six IVFC levels.
3. Update RomFS: hactool merges the base Program NCA with the update's BKTR section.

Steps 1 and 3 run for every file in `input.updates`. Each update's version is identified by the SHA-256 of its `main`,
not by its file name. Output: build/extracted/v0/{exefs,romfs} and build/extracted/v<version>/{exefs,romfs}.

--base, --update, --keys and --work name the files and the output directory instead of the config (the installer
imports the functions instead). Runs under .venv-py314. Key values are read by the tools from the keys file and never
printed.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
# The vendored scripts import each other by bare module name, so their directory goes on the path before importing them.
sys.path.insert(0, str(VENDOR))
from extract_exefs import (common_tickets, container_entries, load_keys, nca_inventory, program_nca,  # noqa: E402
                           record_offset, require)
from extract_romfs import index_romfs  # noqa: E402
from extract_update_romfs import copy_range, hactool_prod_keys  # noqa: E402

DATA = ROOT / 'tools' / 'data'
EXEFS_CODE = ('main', 'sdk', 'subsdk0', 'rtld')
LONG_HEX = re.compile(r'[0-9A-Fa-f]{32,}')
# hactool checks two signatures with Nintendo public keys it does not ship, so these two lines always read (FAIL) and
# say nothing about the data. The IVFC levels and the image hash decide.
UNSIGNED = re.compile(r'\s*(?:Fixed-Key Signature|NPDM Signature) \(FAIL\)')
HACTOOL_ERROR = re.compile(r'\(FAIL\)|\(INVALID\)|Failed to|Error:')
# hactool lines of a hash check that failed: a RomFS level, a section's superblock or its hash table.
HACTOOL_HASH_FAIL = re.compile(r'^\s*(?:Level \d|Superblock Hash|Hash Table) \(FAIL\)', re.M)
# Errors of the vendored extractors (tools/vendor/macnativegames) that mean a game file's data does not match its own
# hashes or ends early: the file is damaged or was not copied completely. DAMAGE finds them in a script's log.
DAMAGE_TEXT = re.compile(r'hash mismatch|hash-table mismatch|FS header hash|IVFC level \d+ block \d+ failed|Truncated|'
                         r'out of bounds|Section bounds')
DAMAGE = re.compile(rf'^\w*Error: (.*(?:{DAMAGE_TEXT.pattern}).*)$', re.M)


class DamagedFile(ValueError):
    """A game file whose data fails its hash checks or ends early. path: that file; detail: the check that failed."""

    def __init__(self, path, detail):
        self.path, self.detail = Path(path), detail
        super().__init__(f'{self.path} is damaged or incomplete ({detail}). Copy that file again from its source '
                         f'(or dump it again) and retry.')


def say(message):
    print(message, flush=True)


def shown(path):
    """path relative to the repository when it is below it (log lines), else as given."""
    return path.relative_to(ROOT) if path.is_relative_to(ROOT) else path


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def load_expected():
    versions = {}
    for path in sorted(DATA.glob('expected-v*.json')):
        data = json.loads(path.read_text())
        versions[data['display_version']] = data
    return versions


def version_dir(expected):
    return 'v0' if expected['version'] == 0 else 'v' + expected['display_version'].replace('.', '')


# The embeddable Python (a ._pth file) does not put a script's directory on sys.path, so a vendored script that imports
# its siblings fails when started by path. This stub adds the directory, then runs the script as __main__.
RUN_SCRIPT = ('import os, runpy, sys; p = sys.argv[1]; sys.path.insert(0, os.path.dirname(p)); sys.argv = sys.argv[1:]; '
              "runpy.run_path(p, run_name='__main__')")


def run_logged(command, log, source=None):
    """Run a vendored script with its output in log. source: the game file it reads; a failed hash check or a short
    read of that file raises DamagedFile."""
    args = [str(c) for c in command]
    env = dict(os.environ, PYTHONIOENCODING='utf-8')   # its messages may name non-ASCII paths
    with log.open('wb') as out:
        code = subprocess.run([args[0], '-c', RUN_SCRIPT, *args[1:]], stdout=out, stderr=subprocess.STDOUT,
                              env=env).returncode
    if code and source:
        damage = DAMAGE.search(log.read_text(encoding='utf-8', errors='replace'))
        if damage:
            raise DamagedFile(source, damage.group(1).strip())
    require(code == 0, f'{Path(command[1]).name} failed; see {shown(log)}')


def exefs_problems(exefs, expected):
    """Code modules must match byte for byte. main.npdm differs between dumps that a repacking tool re-signed."""
    problems = []
    for name in EXEFS_CODE:
        path = exefs / name
        if not path.is_file() or sha256(path) != expected['exefs_sha256'][name]:
            problems.append(name)
    return problems


def extract_exefs(game, keys, out, logs, *, update):
    exefs = out / 'exefs'
    if exefs.exists():
        shutil.rmtree(exefs)
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, VENDOR / 'extract_exefs.py', game, '--keys', keys, '--output', exefs]
    run_logged(command + (['--update'] if update else []), logs / f'extract_exefs-{out.name}.log', source=game)
    return exefs


def identify_update(exefs, versions):
    digest = sha256(exefs / 'main')
    for expected in versions.values():
        if expected['version'] and expected['exefs_sha256']['main'] == digest:
            return expected
    raise ValueError(f'Update main {digest[:16]}... matches no supported version ({", ".join(versions)})')


def report_npdm(exefs, expected):
    if sha256(exefs / 'main.npdm') != expected['exefs_sha256']['main.npdm']:
        say(f'  main.npdm differs from the reference {expected["display_version"]} dump; only the code modules are used')


def romfs_verified(romfs, expected):
    index = romfs / 'index.json'
    if not index.is_file() or not (romfs / 'romfs.bin').is_file():
        return False
    report = json.loads(index.read_text())
    return report.get('romfs_sha256') == expected['romfs']['sha256'] and report.get('romfs_size') == expected['romfs']['size']


def check_romfs(romfs, expected):
    report = json.loads((romfs / 'index.json').read_text())
    require(report['romfs_size'] == expected['romfs']['size'] and report['romfs_sha256'] == expected['romfs']['sha256'],
            f'RomFS does not match {expected["display_version"]}: size {report["romfs_size"]}, '
            f'sha256 {report["romfs_sha256"][:16]}...')
    say(f'  RomFS {expected["display_version"]}: {report["romfs_size"]:,} bytes, {len(report["files"]):,} files, hash matches')


def extract_base_romfs(out, keys, logs, base=None):
    romfs = out / 'romfs'
    if romfs.exists():
        shutil.rmtree(romfs)
    run_logged([sys.executable, VENDOR / 'extract_romfs.py', '--keys', keys,
                '--manifest', out / 'extraction-manifest.json', '--output', romfs], logs / 'extract_romfs-v0.log',
               source=base)


def program_ncas(base, update, keys_path):
    """(tickets, [(source, record, header)] of the base and the update Program NCA). Raises ValueError for a file
    that is no TotK base game or update, or keys that cannot open it."""
    keys = load_keys(keys_path)
    tickets = {}
    inputs = []
    for source, is_update in ((base, False), (update, True)):
        with source.open('rb') as f:
            kind, entries = container_entries(f, source.stat().st_size)
            require(not is_update or kind == 'nsp', 'Update must be an NSP')
            for rights, key in common_tickets(f, entries).items():
                require(tickets.get(rights, key) == key, 'Conflicting common tickets')
                tickets[rights] = key
            record, header = program_nca(nca_inventory(f, entries, keys), update=is_update)
            rights = header[0x230:0x240]
            require(rights == bytes(16) or rights in tickets, 'Missing common ticket')
            inputs.append((source, record, header))
    return tickets, inputs


def merge_space(base, update, keys_path):
    """Bytes merge_update_romfs needs free next to its output, beyond the merged image itself: the two Program NCAs."""
    _, inputs = program_ncas(base, update, keys_path)
    return inputs[0][1]['size'] + inputs[1][1]['size']


MERGE_PHASES = {'copy': '  copying the base and update Program NCAs', 'merge': '  merging with hactool',
                'diagnose': '  a hash check failed: checking the base Program NCA alone',
                'index': '  hashing and indexing the merged image'}


def run_hactool(command, environment):
    """(exit code, stdout and stderr with long hex strings removed). hactool prints UTF-8 (its manifest)."""
    completed = subprocess.run(command, env=environment, capture_output=True, encoding='utf-8', errors='replace')
    return completed.returncode, LONG_HEX.sub('<hex removed>', completed.stdout + completed.stderr)


def damaged_merge_input(hactool, hactool_keys, environment, base, update, base_nca, log_path):
    """The DamagedFile for a merge whose hash checks failed. The merged RomFS mixes data of both files, so hactool
    verifies the base Program NCA alone: if it fails, the base game file is damaged, else the update file is. The
    output is appended to log_path."""
    code, log = run_hactool([str(hactool), '-y', '--disablekeywarns', '--suppresskeys', '-k', str(hactool_keys),
                             str(base_nca)], environment)
    with log_path.open('a', encoding='utf-8') as f:
        f.write(f'\n---- base Program NCA alone (exit code {code}) ----\n{log}')
    checked = '\n'.join(line for line in log.splitlines() if not UNSIGNED.match(line))
    if HACTOOL_HASH_FAIL.search(checked):
        return DamagedFile(base, f'its game data fails the hash check; see {shown(log_path)}')
    if all(re.search(rf'Level {i} \(GOOD\)', log) for i in range(6)):
        return DamagedFile(update, f'its game data fails the hash check; see {shown(log_path)}')
    return ValueError(f'the game data fails the hash check: the base game file {base} or the update file {update} is '
                      f'damaged or incomplete. Copy them again from their source and retry; see {shown(log_path)}')


def merge_update_romfs(base, update, keys_path, hactool, output, logs, phase=lambda name: say(MERGE_PHASES[name])):
    """extract_update_romfs.merge() without its Darwin-only bounded view of the base: the base Program NCA is copied out.

    Needs room for both Program NCAs (about 17 GB together) and the merged image (about 17 GB). phase(name) is called
    as each part of MERGE_PHASES starts.
    """
    if output.exists():
        shutil.rmtree(output)
    tickets, inputs = program_ncas(base, update, keys_path)
    sections = [i for i in range(4) if inputs[1][2][0x400 + i * 512 + 4] == 4]
    require(len(sections) == 1, 'Expected one BKTR section')
    need = inputs[0][1]['size'] + inputs[1][1]['size'] + 18 * (1 << 30)
    require(shutil.disk_usage(output.parent).free > need, f'Need {need / (1 << 30):.0f} GiB free in {output.parent}')
    log_path = logs / f'merge_romfs-{output.parent.name}.log'
    with tempfile.TemporaryDirectory(prefix='update-merge-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        title_keys = tmp / 'title.keys'
        fd = os.open(title_keys, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            for rights, key in tickets.items():
                f.write(f'{rights.hex()} = {key.hex()}\n')
        hactool_keys = tmp / 'hactool.keys'
        hactool_prod_keys(keys_path, hactool_keys)
        phase('copy')
        base_nca, update_nca = tmp / 'base.nca', tmp / 'update.nca'
        for (source, record, _), target in zip(inputs, (base_nca, update_nca)):
            try:
                copy_range(source, record_offset(record), record['size'], target)
            except ValueError as e:   # the file ends inside its Program NCA
                raise DamagedFile(source, str(e)) from None
        result = tmp / 'result'
        result.mkdir()
        phase('merge')
        command = [str(hactool), '-x', '-y', '--disablekeywarns', '--suppresskeys', '-k', str(hactool_keys),
                   f'--basenca={base_nca}', f'--section{sections[0]}={result / "romfs.bin"}', str(update_nca)]
        environment = dict(os.environ, MNG_TITLE_KEYS=str(title_keys))
        code, log = run_hactool(command, environment)
        log_path.write_text(log, encoding='utf-8')
        checked = '\n'.join(line for line in log.splitlines() if not UNSIGNED.match(line))
        if HACTOOL_HASH_FAIL.search(checked):
            phase('diagnose')
            raise damaged_merge_input(hactool, hactool_keys, environment, base, update, base_nca, log_path)
        require(code == 0 and not HACTOOL_ERROR.search(checked), f'hactool merge failed; see {shown(log_path)}')
        require(all(re.search(rf'Level {i} \(GOOD\)', log) for i in range(6)), 'hactool did not verify all six IVFC levels')
        image = result / 'romfs.bin'
        phase('index')
        files = index_romfs(image)
        report = dict(status='pass', romfs_size=image.stat().st_size, romfs_sha256=sha256(image), files=files,
                      integrity='BKTR merge; all six IVFC levels verified')
        (result / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        result.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.toml')
    parser.add_argument('--hactool', type=Path,
                        default=ROOT / 'toolchain' / 'hactool' / ('hactool.exe' if os.name == 'nt' else 'hactool'))
    parser.add_argument('--base', type=Path, help='base game (default: input.base of the config)')
    parser.add_argument('--update', type=Path, action='append',
                        help='an update NSP, repeatable (default: input.updates of the config)')
    parser.add_argument('--keys', type=Path, help='prod.keys (default: input.keys of the config)')
    parser.add_argument('--work', type=Path, help='output directory with extracted/ and logs/ (default: work.dir of '
                                                  'the config, else build)')
    args = parser.parse_args()

    config = {}
    if not (args.base and args.keys and args.update and args.work):
        config = tomllib.loads(args.config.read_text(encoding='utf-8'))
    given = config.get('input', {})
    base, keys = args.base or Path(given['base']), args.keys or Path(given['keys'])
    updates = args.update or given.get('updates', [])
    work = args.work or ROOT / config.get('work', {}).get('dir', 'build')
    extracted, logs = work / 'extracted', work / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    versions = load_expected()
    v0 = versions['1.0.0']

    say('Base game')
    base_out = extracted / 'v0'
    if exefs_problems(base_out / 'exefs', v0):
        say('  extracting ExeFS')
        extract_exefs(base, keys, base_out, logs, update=False)
    problems = exefs_problems(base_out / 'exefs', v0)
    require(not problems, f'Base game is not TotK 1.0.0: {", ".join(problems)} differ')
    say('  ExeFS 1.0.0: main, sdk, subsdk0, rtld match')
    report_npdm(base_out / 'exefs', v0)
    if not romfs_verified(base_out / 'romfs', v0):
        say('  extracting RomFS (17 GB)')
        extract_base_romfs(base_out, keys, logs, base)
    check_romfs(base_out / 'romfs', v0)

    for update in updates:
        extract_update(base, Path(update), keys, args.hactool, extracted, logs, versions)


def extract_update(base, update, keys, hactool, extracted, logs, versions):
    say(f'Update {update.name}')
    staging = extracted / 'update-staging'
    known = [d for d in extracted.glob('v*') if d.name != 'v0' and (d / 'extraction-manifest.json').is_file()]
    current = next((d for d in known if json.loads((d / 'extraction-manifest.json').read_text())['source']
                    == str(update.resolve())), None)
    if current is None or not any(not exefs_problems(current / 'exefs', e) for e in versions.values() if e['version']):
        say('  extracting ExeFS')
        exefs = extract_exefs(update, keys, staging, logs, update=True)
        expected = identify_update(exefs, versions)
        current = extracted / version_dir(expected)
        if current.exists():
            shutil.rmtree(current)
        staging.rename(current)
    expected = identify_update(current / 'exefs', versions)
    problems = exefs_problems(current / 'exefs', expected)
    require(not problems, f'Update {expected["display_version"]}: {", ".join(problems)} differ')
    say(f'  ExeFS {expected["display_version"]}: main, sdk, subsdk0, rtld match')
    report_npdm(current / 'exefs', expected)
    if not romfs_verified(current / 'romfs', expected):
        say('  merging RomFS with the update')
        merge_update_romfs(base, update, keys, hactool, current / 'romfs', logs)
    check_romfs(current / 'romfs', expected)


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        sys.exit(f'error: {error}')
