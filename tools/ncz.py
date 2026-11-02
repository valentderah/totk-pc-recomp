"""Unpack an NSZ or XCZ (a game file compressed by the nsz tool) into a plain NSP.

An NSZ is an NSP whose NCAs are stored as NCZ files: the first 0x4000 bytes of the NCA (its encrypted header) as they
are, then an NCZSECTN table of the NCA's sections with the AES-CTR key and counter of each, then the rest of the NCA
decrypted and compressed with zstd, either as one stream or as an NCZBLOCK table of independently compressed blocks.
An XCZ is an XCI whose secure partition holds NCZ files. Unpacking encrypts the sections again, so every NCA comes back
byte for byte; its file name is the first half of its SHA-256, which is checked. The output is an NSP of the secure
partition (an XCZ) or of the NSZ's files, with .ncz renamed to .nca. Format: nicoboss/nsz, nsz/NszDecompressor.py
(__decompressNcz) and nsz/BlockDecompressorReader.py; this is our own code.

usage: ncz.py <file.nsz|file.xcz> <out.nsp>. Runs under .venv-py314 (compression.zstd).
"""
import hashlib
import io
import struct
import sys
from compression import zstd
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, str(Path(__file__).resolve().parent / 'vendor' / 'macnativegames'))
from extract_exefs import container_entries, read_at, require  # noqa: E402

HEADER = 0x4000        # the NCA header, kept encrypted and uncompressed
CHUNK = 1 << 20
CRYPTO_CTR = (3, 4)    # AES-CTR and BKTR sections; the other types are stored as they are


class DamagedFile(ValueError):
    """An NCZ whose unpacked NCA does not match its name's hash, or whose data ends early."""

    def __init__(self, path, detail):
        self.path, self.detail = Path(path), detail
        super().__init__(f'{self.path} is damaged or incomplete ({detail})')


def compressed(path):
    """('nsz' or 'xcz', entries) for a game file holding NCZ files, else None. Raises ValueError for a file that is
    neither an NSP nor an XCI."""
    with Path(path).open('rb') as f:
        kind, entries = container_entries(f, Path(path).stat().st_size)
    if not any(name.lower().endswith('.ncz') for name, _, _ in entries):
        return None
    return ('nsz' if kind == 'nsp' else 'xcz'), entries


def sections(f, start):
    """[(offset, size, crypto type, key, counter)] of the NCZ at start, and the offset of its data after the table."""
    head = read_at(f, start + HEADER, 16)
    require(head[:8] == b'NCZSECTN', 'NCZ without its NCZSECTN table')
    count = struct.unpack_from('<Q', head, 8)[0]
    require(0 < count <= 0x10000, 'Invalid NCZ section count')
    raw = read_at(f, start + HEADER + 16, count * 0x40)
    out = []
    for i in range(count):
        offset, size, crypto = struct.unpack_from('<QQQ', raw, i * 0x40)
        out.append((offset, size, crypto, raw[i * 0x40 + 0x20:i * 0x40 + 0x30], raw[i * 0x40 + 0x30:i * 0x40 + 0x40]))
    out.sort()
    require(out[0][0] >= HEADER, 'NCZ section inside the NCA header')
    for (a, size, *_), (b, *_) in zip(out, out[1:]):
        require(a + size <= b, 'Overlapping NCZ sections')
    return out, start + HEADER + 16 + count * 0x40


def nca_size(f, start):
    table, _ = sections(f, start)
    return table[-1][0] + table[-1][1]


def layout(source):
    """(entries, NSP header, NSP size) of the NSP that unpack writes for the NSZ or XCZ source: its files in order,
    .ncz renamed to .nca, the PFS0 table padded to 0x20 bytes."""
    _, entries = compressed(source)
    with Path(source).open('rb') as f:
        sizes = [nca_size(f, start) if name.lower().endswith('.ncz') else length for name, start, length in entries]
    names = [name[:-4] + '.nca' if name.lower().endswith('.ncz') else name for name, _, _ in entries]
    strings = b''.join(n.encode('utf-8') + b'\0' for n in names)
    strings += bytes(-(16 + 0x18 * len(names) + len(strings)) % 0x20)
    head = b'PFS0' + struct.pack('<III', len(names), len(strings), 0)
    offset, name_at = 0, 0
    for name, size in zip(names, sizes):
        head += struct.pack('<QQII', offset, size, name_at, 0)
        offset += size
        name_at += len(name.encode('utf-8')) + 1
    head += strings
    return entries, head, len(head) + offset


def unpacked_size(source):
    """Bytes of the NSP that unpack writes for the NSZ or XCZ source."""
    return layout(source)[2]


class Slice(io.RawIOBase):
    """Bytes start..end of a file, for zstd.ZstdFile: a stream may hold several frames and must stop at the entry's
    end."""

    def __init__(self, f, start, end):
        self.f, self.pos, self.end = f, start, end

    def readable(self):
        return True

    def readinto(self, buffer):
        n = min(len(buffer), self.end - self.pos)
        if n <= 0:
            return 0
        self.f.seek(self.pos)
        data = self.f.read(n)
        buffer[:len(data)] = data
        self.pos += len(data)
        return len(data)


def blocks(f, pos, end, total):
    """The NCZBLOCK data at pos, decompressed, in pieces. A block whose stored size is not smaller than its
    decompressed size is stored as it is."""
    head = read_at(f, pos, 24)
    exponent, count = head[11], struct.unpack_from('<I', head, 12)[0]
    size = struct.unpack_from('<Q', head, 16)[0]
    require(head[8] == 2 and head[9] == 1 and 14 <= exponent <= 32, 'Unsupported NCZBLOCK header')
    require(size == total and count == -(-size // (1 << exponent)), 'NCZBLOCK sizes do not match the sections')
    stored = struct.unpack_from(f'<{count}I', read_at(f, pos + 24, 4 * count))
    pos += 24 + 4 * count
    for i, length in enumerate(stored):
        plain = min(1 << exponent, size - (i << exponent))
        require(pos + length <= end, 'Truncated NCZ block')
        data = read_at(f, pos, length)
        pos += length
        yield zstd.decompress(data) if length < plain else data


def stream(f, pos, end, total):
    """The decompressed NCA body after the header, in pieces of up to CHUNK bytes."""
    if read_at(f, pos, 8) == b'NCZBLOCK':
        yield from blocks(f, pos, end, total)
        return
    with zstd.ZstdFile(io.BufferedReader(Slice(f, pos, end), CHUNK)) as z:
        while data := z.read(CHUNK):
            yield data


def unpack_ncz(f, start, end, out, name, source, progress):
    """Write the NCA of the NCZ at start..end to out; check it against name's hash. progress(bytes) after each piece."""
    table, pos = sections(f, start)
    total = table[-1][0] + table[-1][1]
    digest = hashlib.sha256()
    header = read_at(f, start, HEADER)
    out.write(header)
    digest.update(header)
    progress(HEADER)
    at = HEADER
    pending = b''
    try:
        pieces = stream(f, pos, end, total - HEADER)
        for piece in pieces:
            pending += piece
            while pending and at < total:
                section = next((s for s in table if s[0] <= at < s[0] + s[1]), None)
                stop = section[0] + section[1] if section else next((s[0] for s in table if s[0] > at), total)
                n = min(len(pending), stop - at)
                crypto = section and section[2] in CRYPTO_CTR
                if crypto and at + n < stop:
                    n -= n % 16   # the counter of the next piece starts at a whole AES block
                    if not n:
                        break
                data, pending = pending[:n], pending[n:]
                if crypto:
                    require(at % 16 == 0, 'Unaligned NCZ section')
                    counter = section[4][:8] + struct.pack('>Q', at >> 4)
                    data = Cipher(algorithms.AES(section[3]), modes.CTR(counter)).encryptor().update(data)
                out.write(data)
                digest.update(data)
                at += n
                progress(n)
            if at == total:
                break
    except (zstd.ZstdError, EOFError) as e:   # EOFError: the stream ends before its end-of-frame marker
        raise DamagedFile(source, f'{name}: {e}') from None
    if at != total or pending:
        raise DamagedFile(source, f'{name}: {at - HEADER:,} of {total - HEADER:,} bytes')
    stem = name[:-4]
    if len(stem) == 32 and digest.hexdigest()[:32] != stem.lower():
        raise DamagedFile(source, f'{name}: hash mismatch')


def unpack(source, target, progress=lambda done, total: None):
    """Write the NSP of the NSZ or XCZ source to target (through target.part). progress(done, total) in bytes."""
    source, target = Path(source), Path(target)
    kind = compressed(source)[0]
    entries, head, total = layout(source)
    part = target.with_name(target.name + '.part')
    done = 0

    def step(n):
        nonlocal done
        done += n
        progress(done, total)

    with source.open('rb') as f, part.open('wb') as out:
        out.write(head)
        step(len(head))
        for name, start, length in entries:
            if name.lower().endswith('.ncz'):
                unpack_ncz(f, start, start + length, out, name, source, step)
                continue
            for at in range(start, start + length, CHUNK):
                data = read_at(f, at, min(CHUNK, start + length - at))
                out.write(data)
                step(len(data))
    part.replace(target)
    return kind


def main():
    source, target = sys.argv[1:3]
    shown = [-1]

    def progress(done, total):
        percent = done * 100 // total
        if percent != shown[0]:
            shown[0] = percent
            print(f'\r{percent}%', end='', flush=True)

    kind = unpack(source, target, progress)
    print(f'\n{kind}: wrote {target}')


if __name__ == '__main__':
    main()
