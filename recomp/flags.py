"""Function-local NZCV for recomp/gen.py.

The lifter writes flags the way the guest does: every flag-setting instruction packs N, Z, C and V into c->nzcv
(subs64() and friends in runtime/cpu.h) and every reader unpacks them (COND_xx(c)). Compilers keep NZCV inside a
function, and AAPCS64 neither passes it to a callee nor preserves it across a call. So a generated function keeps its
flags in a local `Flags F` (runtime/cpu.h), which clang holds in registers, and writes c->nzcv only where it leaves
for another function other than by a call or a return (see plan()).

A flag-setting instruction of a known kind (subtract, add or logic result; 32 or 64 bits) stores its operands in F.a
and F.b. A reader all of whose reaching definitions are of one kind computes its condition from those operands, so
`cmp x1, x2; b.lt` becomes a signed compare in C. Instructions whose result is only known packed (CCMP, FCMP, MSR
NZCV, ADCS) write F.nz, the packed form; a reader whose definitions are of several kinds reads F.nz, and those
definitions then also write it.

A read that can see flags a callee left reads the caller's flags from before the call instead. AAPCS64 code never
reads a callee's flags, so such a read only sits on a path that does not run: in TotK it follows calls to noreturn
functions (asserts) that the compiler placed before another branch's target. Plan.after_call counts them.

A mod plugin's inline hook breaks that rule. exlaunch's exl_inline_hook_impl saves and restores x0..x29 around the
callback but not NZCV, so the hooked game code goes on with the flags the callback left (Challenge Mode's callbacks end
in a cmp, and the b.eq at their sites follows it). So in a plugin module flags pass through calls and returns (plan's
`through`), and a hook site takes the flags the stub's call left (plan's `hooks`).
"""
import re

KIND_DEF = re.compile(r'\b(subs|adds|logic)(32|64)\(c, ')
CARRY_DEF = re.compile(r'\b(adcs|sbcs)(32|64)\(c, ')
CARRY_USE = re.compile(r'\b(adc|sbc)(32|64)\(c, ')
COND = re.compile(r'COND_([A-Z]{2})\(c\)')
NZ_DEF = 'c->nzcv = '
NZ_USE = '(c->nzcv)'
KIND_NAME = {'subs': 'sub', 'adds': 'add', 'logic': 'logic'}

ENTRY, CALL, HOOK = 'entry', 'call', 'hook'   # HOOK: (HOOK, i), the callback's flags at hook site i


class FlagError(Exception):
    pass


def effect(code):
    """(reads flags, kind of the flags written or None) of one instruction's C."""
    reads = bool(COND.search(code) or CARRY_USE.search(code) or CARRY_DEF.search(code) or NZ_USE in code)
    if NZ_DEF in code or CARRY_DEF.search(code):
        return reads, 'nz'
    defs = KIND_DEF.findall(code)
    if len(defs) > 1:
        raise FlagError(f'several flag definitions in one instruction: {code}')
    if defs:
        op, size = defs[0]
        return reads, KIND_NAME[op] + size
    return reads, None


class Plan:
    """Per instruction: the kind its flag reads use, whether its flag definition also writes F.nz, and what it writes
    to c->nzcv before it leaves for another function."""

    def __init__(self, n):
        self.reader = [None] * n
        self.pack = [False] * n
        self.exit = [None] * n      # kind of the flags to publish in c->nzcv at a tail exit, or None
        self.pre = [None] * n       # kind of the flags to publish before a call whose callee sees them, or None
        self.takes = [False] * n    # the call leaves its callee's flags (F.nz from c->nzcv after it)
        self.entry = False          # F.nz starts as c->nzcv: flags from the entry reach a read or an exit
        self.entry_read = False     # they reach a read, not only an exit
        self.after_call = 0         # reads that can see flags a callee left (see the module docstring)
        self.used = False

    def decl(self):
        """Declaration of F at the top of the function."""
        return 'Flags F = {.nz = c->nzcv};' if self.entry else 'Flags F = {0};'

    def publish(self, i):
        """Statement writing the flags to c->nzcv at instruction i's tail exit ('' when it need not)."""
        k = self.exit[i]
        return f'c->nzcv = fnz_{k}(&F); ' if k else ''

    def give(self, i):
        """Statement writing the flags to c->nzcv before instruction i's call, whose callee sees them (plan's through
        and hooks; '' when it need not)."""
        k = self.pre[i]
        return f'c->nzcv = fnz_{k}(&F); ' if k else ''

    def take(self, i):
        """Statement taking the flags the callee of instruction i's call left (plan's through and hooks)."""
        return ' F.nz = c->nzcv;' if self.used and self.takes[i] else ''

    def rewrite(self, i, code):
        """code of instruction i with its flag reads and writes on F."""
        rk = self.reader[i]
        _, dk = effect(code)
        if rk:
            code = COND.sub(lambda m: f'fc_{rk}(&F, CC_{m.group(1)})', code)
            code = CARRY_DEF.sub(lambda m: f'{m.group(1)}{m.group(2)}f(&F, fc_{rk}(&F, CC_CS), ', code)
            code = CARRY_USE.sub(lambda m: f'{m.group(1)}{m.group(2)}f(fc_{rk}(&F, CC_CS), ', code)
            code = code.replace(NZ_USE, f'(fnz_{rk}(&F))')
        if dk == 'nz':
            code = code.replace(NZ_DEF, 'F.nz = ')
            code = re.sub(r'\(void\)(subs|adds)(32|64)\(c, ', lambda m: f'F.nz = nz_{KIND_NAME[m.group(1)]}{m.group(2)}(',
                          code)
        elif dk:
            code = KIND_DEF.sub(lambda m: f'f{KIND_NAME[m.group(1)]}{m.group(2)}(&F, ', code)
            if self.pack[i]:
                code += f' F.nz = fnz_{dk}(&F);'
        if 'c->nzcv' in code or COND.search(code) or KIND_DEF.search(code) or CARRY_USE.search(code):
            raise FlagError(f'flag access left after the rewrite: {code}')
        return code


def plan(effects, succ, calls, exits, through=(), hooks=()):
    """effects[i]: (reads, kind or None); succ[i]: successor indices inside the function; calls: indices of calls,
    whose callee may leave any flags; exits: indices that can leave for another function other than by a call or a
    return (a tail branch, falling into the next function, a br through rt_dispatch).

    Discovery can start a function inside another one (a branch target, a materialised address), and the code
    before it then falls or branches into it with live flags. So a tail exit publishes the flags in c->nzcv, and a
    function whose reads can see its entry flags starts F.nz from c->nzcv. Calls and returns publish nothing: AAPCS64
    code neither passes flags to a callee nor reads a callee's, so a call's flags count as no definition.

    through: calls whose flags pass both ways (a plugin module, see the module docstring; its returns are in exits):
    each publishes the flags before the call (Plan.give) and defines F.nz from c->nzcv after it (Plan.take).
    hooks: patched sites with a plugin inline hook before the site's own word, when the hook's set is on: the hook
    publishes the flags before its call and leaves the callback's after it, which the site's own word reads unless it
    writes flags itself. With the set off the flags are the ones before the site, so what follows can see both."""
    n = len(effects)
    p = Plan(n)
    if not any(r or k for r, k in effects):
        return p
    p.used = True
    hooks = set(hooks) - set(calls)   # a hook before a bl: the bl's callee leaves the flags
    reach = [set() for _ in range(n)]
    reach[0].add(ENTRY)
    work = [0]
    while work:
        i = work.pop()
        if effects[i][1] or i in through:
            out = {i}
        elif i in calls:
            out = {CALL}
        elif i in hooks:
            out = reach[i] | {(HOOK, i)}
        else:
            out = reach[i]
        for s in succ[i]:
            if not out <= reach[s]:
                reach[s] |= out
                work.append(s)

    def kind(d):
        if d == ENTRY or isinstance(d, tuple):
            return 'nz'
        return effects[d][1] or 'nz'   # a through call leaves its callee's flags packed

    def use(r):
        """Kind a use of the definitions r reads; several kinds read the packed form, which they then all write."""
        kinds = {kind(d) for d in r}
        if ENTRY in r:
            p.entry = True
        if len(kinds) == 1:
            return kinds.pop()
        for d in r:
            if isinstance(d, int) and effects[d][1]:
                p.pack[d] = True
        return 'nz'   # several kinds, or none (unreachable)

    def seen(i):
        """What a read or an exit at instruction i sees: at a hook site also the callback's flags."""
        return reach[i] | {(HOOK, i)} if i in hooks else reach[i]

    for i, (reads, _) in enumerate(effects):
        if reads:
            p.after_call += CALL in reach[i]
            p.reader[i] = use(seen(i) - {CALL})
            p.entry_read |= ENTRY in reach[i]
    for i in exits:
        # Entry flags alone are still in c->nzcv.
        r = seen(i) - {CALL}
        if r and r != {ENTRY}:
            p.exit[i] = use(r)
    for i in set(through) | hooks:
        p.takes[i] = True
        r = reach[i] - {CALL}
        if r and r != {ENTRY}:
            p.pre[i] = use(r)
    return p
