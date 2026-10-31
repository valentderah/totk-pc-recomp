"""Select TotK content in combined containers without changing vendored readers.

External title.keys entries use the same encrypted title keys as common tickets.
They stay in memory and are never written into temporary files.
The existing readers still verify every ExeFS and RomFS hash.
"""
import argparse
from pathlib import Path
import sys

VENDOR = Path(__file__).resolve().parent / 'vendor/macnativegames'
sys.path.insert(0, str(VENDOR))
import extract_exefs as exefs_reader
from extract_exefs import common_tickets, load_keys, nca_inventory, require

TITLE = '0100F2C0115B6000'
UPDATE_TITLE = '0100F2C0115B6800'


def select_program(records, update=False):
    require(all(record['title_id'] in (TITLE, UPDATE_TITLE) for record, _ in records),
            'Container includes content from another title')
    programs = [(record, header) for record, header in records if record['content_type'] == 0]
    require(all(record['title_id'] == TITLE for record, _ in programs), 'Unexpected Program title ID')
    groups = {False: [], True: []}
    for record, header in programs:
        patched = any(header[0x400 + i * 512 + 4] == 4 for i in range(4))
        groups[patched].append((record, header))
    require(all(len(group) <= 1 for group in groups.values()), 'Ambiguous container: multiple base games or updates')
    require(len(groups[update]) == 1, 'Container has no ' + ('BKTR update' if update else 'base Program NCA'))
    return groups[update][0]


class InputKeys:
    def __init__(self, system_path, title_path=None):
        self.system = load_keys(Path(system_path))
        self.titles = load_keys(Path(title_path)) if title_path else {}
        for name, value in self.titles.items():
            require(len(name) == 32 and all(c in '0123456789abcdef' for c in name) and len(value) == 16,
                    'Invalid title.keys entry; expected a rights ID and a 16-byte title key')

    def tickets(self, stream, entries):
        tickets = common_tickets(stream, entries)
        if not self.titles:
            return tickets
        for _, header in nca_inventory(stream, entries, self.system):
            rights = header[0x230:0x240]
            title = self.titles.get(rights.hex())
            if rights == bytes(16) or title is None:
                continue
            require(rights not in tickets or tickets[rights] == title,
                    'Common ticket conflicts with the supplied title.keys')
            tickets[rights] = title
        return tickets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('exefs', 'romfs'))
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--title-keys', type=Path)
    args, remaining = parser.parse_known_args()
    keys = InputKeys(args.keys, args.title_keys)
    if args.mode == 'exefs':
        reader = exefs_reader
    else:
        import extract_romfs as reader
    # Each adapter runs in its own process. Only container selection and key
    # lookup change; the upstream decryption and integrity checks remain intact.
    reader.program_nca = select_program
    reader.common_tickets = keys.tickets
    sys.argv = [reader.__file__, '--keys', str(args.keys), *remaining]
    reader.main()


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        raise SystemExit(f'ValueError: {error}') from None
