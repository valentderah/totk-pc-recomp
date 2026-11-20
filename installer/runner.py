"""Which steps run, in what order, and the bookkeeping around them: fresh install, resume and update are one rule.

Each step has a key: a fingerprint of everything its output depends on, computed before anything runs (a step's key
includes the keys of the steps it reads from, so a change upstream reaches every step below it). A step is skipped
when totk-install.json records it with the same key and its output still matches the record (Step.current, cheap).
Otherwise it runs. A transient step (its output lives in the temp folder and only the next step reads it) runs only
when the next step runs. `always` steps (check, cleanup) run every time.

Update mode is the same rule with a newer release: the extraction's key depends only on the expected hashes, so a
release that changes the lifter or the runtime keeps romfs/ and exefs/ and rebuilds from lift on. Before keeping the
output of a step recorded by another release (or with --verify), the check step calls Step.verify (expensive: the full
RomFS hash); a mismatch makes the step run again.

A failed or cancelled run leaves the finished steps recorded; the next run starts at the first step that is not.
"""
import datetime
import traceback

from . import state as statefile


class InstallError(Exception):
    """A failure with a message id (installer/text/en.json) and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


class Step:
    id = ''
    transient = False
    always = False

    def key(self, ctx):
        """Fingerprint of the step's inputs; may read ctx.keys of earlier steps."""
        return ''

    def current(self, ctx, record):
        """True when the recorded output is still in place (cheap checks only)."""
        return True

    def verify(self, ctx, record):
        """True when the recorded output passes the full check (hashes); called before keeping another release's
        output."""
        return True

    def run(self, ctx):
        """Do the work. Returns fields to record with the step."""
        return {}

    def settle(self, ctx):
        """After the step ran or was skipped: a step whose output the later steps run from (the build kit) points the
        context at it."""


class Context:
    def __init__(self, layout, dest, temp, reporter, options, state=None, verify=False):
        self.layout, self.dest, self.temp, self.reporter, self.options = layout, dest, temp, reporter, options
        self.state = state or statefile.empty(layout.release)
        self.verify = verify
        self.mode = 'install' if state is None else 'resume' if state.get('release') == layout.release else 'update'
        self.keys, self.runs, self.reasons, self.steps, self.forced = {}, {}, {}, [], set()

    def record(self, step_id):
        return self.state['steps'].get(step_id)

    def will_run(self, step_id):
        return self.runs.get(step_id, False)

    def save(self):
        self.dest.mkdir(parents=True, exist_ok=True)
        statefile.save(self.dest, self.state)

    def plan(self, forced=()):
        """Fill keys (once), runs and reasons. forced: step ids that must run."""
        if not self.keys:
            for s in self.steps:
                self.keys[s.id] = s.key(self)
        for s in self.steps:
            rec = self.record(s.id)
            if s.always or s.id in forced:
                self.runs[s.id] = True
            else:
                self.runs[s.id] = not (rec and rec.get('key') == self.keys[s.id] and s.current(self, rec))
            self.reasons[s.id] = '' if self.runs[s.id] else 'current'
        for i in range(len(self.steps) - 2, -1, -1):
            s, after = self.steps[i], self.steps[i + 1]
            if s.transient and self.runs[s.id] and not self.runs[after.id]:
                self.runs[s.id], self.reasons[s.id] = False, 'not_needed'
        self.forced = set(forced)

    def verify_kept(self, progress_step):
        """Full check of every kept step output that another release recorded (or of all with --verify). A step that
        fails it is planned to run. Returns the ids that failed."""
        failed = []
        for s in self.steps:
            rec = self.record(s.id)
            if self.runs[s.id] or self.reasons[s.id] != 'current' or s.always:
                continue
            if not (self.verify or rec.get('release') != self.layout.release):
                continue
            if not s.verify(self, rec):
                failed.append(s.id)
        if failed:
            for f in failed:
                self.reporter.warning('state.output_changed', step=f)
            self.plan(self.forced | set(failed))
        return failed


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def run_steps(steps, ctx):
    """Run the plan. Returns 'ok', 'failed' or 'cancelled'; events and the log say why."""
    rep = ctx.reporter
    ctx.steps = steps
    current = None
    try:
        ctx.plan()
        rep.emit('plan', run=[s.id for s in steps if ctx.runs[s.id]],
                 skip={s.id: ctx.reasons[s.id] for s in steps if not ctx.runs[s.id]})
        for s in steps:
            if not ctx.runs[s.id]:
                rep.step(s.id, 'skipped', reason=ctx.reasons[s.id])
                s.settle(ctx)
                continue
            current = s.id
            rep.step(s.id, 'started')
            t0 = rep.elapsed()
            fields = s.run(ctx) or {}
            seconds = round(rep.elapsed() - t0, 1)
            if not s.always:
                ctx.state['steps'][s.id] = {'key': ctx.keys[s.id], 'release': ctx.layout.release,
                                            'finished': now_utc(), 'seconds': seconds, **fields}
            ctx.save()   # the first save, after the check, creates the install folder
            rep.step(s.id, 'done', seconds=seconds)
            current = None
            s.settle(ctx)
        ctx.state['release'] = ctx.layout.release
        ctx.save()
        return 'ok'
    except InstallError as e:
        rep.error(e.id, e.params, step=current)
        return 'failed'
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)}, step=current)
        return 'failed'
    except KeyboardInterrupt:
        rep.log(f'cancelled during {current}')
        return 'cancelled'
    except Exception as e:
        rep.log(traceback.format_exc())
        rep.error('internal', {'exception': type(e).__name__, 'detail': str(e)}, step=current)
        return 'failed'
