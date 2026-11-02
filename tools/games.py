"""The games this repository builds: their title IDs, build version names and expected-hash files.

Tears of the Kingdom builds are named v<digits of the display version> (v121, v143) and their hashes are
tools/data/expected-v<display version>.json; Breath of the Wild builds are named botw<digits> (botw190) and their hashes
are tools/data/expected-botw-v<display version>.json. The runtime tells the games apart by rt_game (runtime/host.h),
which tools/build_game.py writes into modules.c.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Game:
    key: str        # 'totk', 'botw'
    name: str
    runtime: int    # rt_game: RT_GAME_TOTK, RT_GAME_BOTW (runtime/host.h)
    base_id: str    # the base game's (and its Program NCA's) title ID
    update_id: str  # the update's title ID
    prefix: str     # build version names: prefix + the display version's digits
    data: str       # tools/data/<data><display version>.json

    def version_name(self, display):
        return self.prefix + display.replace('.', '')

    def program_nca(self, records, update=False):
        """The Program NCA of a base game or update package of this game, as program_nca in
        tools/vendor/macnativegames/extract_exefs.py picks it for Tears of the Kingdom (that file stays as upstream has
        it)."""
        def require(ok, message):
            if not ok:
                raise ValueError(message)
        programs = [(r, h) for r, h in records if r['content_type'] == 0]
        if update:
            require(len(programs) == 1, 'Expected one update Program NCA; combined packages are unsupported')
            record, hdr = programs[0]
            require(record['title_id'] == self.base_id, f'Update belongs to another game, not {self.name}')
            require(any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4)), 'Expected a BKTR update, not a base game')
            require(all(r['title_id'] in (self.base_id, self.update_id) for r, _ in records),
                    'Mixed-title update package')
            return record, hdr
        # Update Program NCAs can retain the base title ID; their Meta NCA carries the update ID.
        metadata = [r for r, _ in records if r['content_type'] == 1]
        require(len(metadata) <= 1 and all(r['title_id'] == self.base_id for r in metadata),
                'Expected base game metadata; updates, DLC and combined packages are unsupported')
        require(len(programs) == 1, 'Expected one base Program NCA; updates, DLC and combined packages are unsupported')
        record, hdr = programs[0]
        require(record['title_id'] == self.base_id, f'Expected the {self.name} base game; other games are unsupported')
        return record, hdr


TOTK = Game('totk', 'Tears of the Kingdom', 0, '0100F2C0115B6000', '0100F2C0115B6800', 'v', 'expected-v')
BOTW = Game('botw', 'Breath of the Wild', 1, '01007EF00011E000', '01007EF00011E800', 'botw', 'expected-botw-v')
GAMES = {g.key: g for g in (TOTK, BOTW)}

# Build version name -> (game, display version). v0 is Tears of the Kingdom's base game.
VERSIONS = {'v0': (TOTK, '1.0.0'), 'v110': (TOTK, '1.1.0'), 'v112': (TOTK, '1.1.2'), 'v121': (TOTK, '1.2.1'),
            'v143': (TOTK, '1.4.3'), 'botw190': (BOTW, '1.9.0')}


def by_title(title_id):
    """The game whose base or update title ID this is, None when no game here has it."""
    t = title_id.upper()
    return next((g for g in GAMES.values() if t in (g.base_id, g.update_id)), None)
