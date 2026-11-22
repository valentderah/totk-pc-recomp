"""Condense the TOTK_TRACE=perf and TOTK_TRACE=sched lines of a game log over a frame window.

    python tools/perf_summary.py build/v121/logs/fight.log 1560 2600

`mem: dirty` lines (vk_memory.c dirty census) are summarised per pool class (CPU_CACHED, CPU_UNCACHED, other): what
a device-local mirror of buffer-bound pools would upload per frame and per submit, FlushMappedRange use, mirror size.
"""
import re
import statistics
import sys

PERF = re.compile(r'perf: frame (\d+): interval avg ([\d.]+) ms max ([\d.]+) ms \(([\d.]+) fps\); render thread blocked: '
                  r'pacing (\d+)%.*?pipeline build (\d+)%.*?texture (\d+)%')
FRAME_COUNT = re.compile(r'; ab \d+/(\d+);')
GPU = re.compile(r'perf: frame (\d+): gpu avg (?:([\d.]+) ms|n/a).*?fence wait per frame: render thread ([\d.]+) ms, '
                 r'other threads ([\d.]+) ms; vram (?:([\d.]+) of ([\d.]+) MiB|n/a).*?ram working set ([\d.]+) MiB, '
                 r'private ([\d.]+) MiB; bound: (\w+)')
SCHED = re.compile(r'sched: frame (\d+): (.*)')
THREAD = re.compile(r'sched: (render )?(0x[0-9a-f]+) (p\d+ m\d+) run (\d+)% core (\d+)% host (\d+)% sleep (\d+)% '
                    r'sync (\d+)% cv (\d+)% lock (\d+)% addr (\d+)% cpu (\d+)% blocks (\d+) preempted (\d+)')
RWAIT = re.compile(r'sched: render wait (\d): (.*)')
DIRTY = re.compile(r'mem: dirty frame (\d+): (.*)')
DIRTY_FRAMES = re.compile(r'mem: dirty frames KiB [\w/]+:(.*)')
MEMCLASS = re.compile(r'mem: (class .*)')
CLASSES = ('cached', 'uncached', 'other')


def dirty_fields(text):
    """`key a/b/c` and `key x` pairs of a `mem: dirty frame` line -> {key: [floats]}."""
    return {k: [float(x) for x in v.split('/')] for k, v in re.findall(r'(\w+) ([\d.]+(?:/[\d.]+)*)(?=[ ;]|$)', text)}


def p95(v):
    v = sorted(v)
    return v[min(len(v) - 1, int(0.95 * len(v)))] if v else 0.0


def dirty_summary(lines, frames):
    """lines: [(frame, fields)] of the window; frames: per-frame eager KiB triples."""
    if not lines:
        return
    n = sum(f['frames'][0] for _, f in lines)

    def avg(key, i):   # frame-weighted mean of a per-frame value
        return sum(f[key][i] * f['frames'][0] for _, f in lines if key in f) / n

    for i, c in enumerate(CLASSES):
        per = [t[i] / 1024 for t in frames]
        print(f'SUMMARY dirty {c}: eager MiB/frame avg {avg("eager_mib", i):.3f} p95 {p95(per):.3f} '
              f'max {max(per, default=0):.3f}; lazy avg {avg("lazy_mib", i):.3f}; first use avg {avg("first_mib", i):.3f}; '
              f'FlushMappedRange pool {avg("flush_pool_calls", i):.1f} calls {avg("flush_pool_mib", i):.3f} MiB, '
              f'buffer {avg("flush_buf_calls", i):.1f} calls {avg("flush_buf_mib", i):.3f} MiB per frame')
    tot = [sum(t) / 1024 for t in frames]
    subs = avg('submits_per_frame', 0)
    last = lines[-1][1]
    print(f'SUMMARY dirty all: eager MiB/frame avg {sum(avg("eager_mib", i) for i in range(3)):.3f} p95 {p95(tot):.3f} '
          f'max {max(tot, default=0):.3f}; submits/frame {subs:.2f}; per submit avg '
          f'{sum(f["avg_mib"][0] * f["frames"][0] for _, f in lines) / n:.3f} MiB max '
          f'{max(f["max_mib"][0] for _, f in lines):.3f} MiB')
    mirror = ', '.join(f'{c} {int(last["live_pools"][i])} pools {last["live_mib"][i]:.1f} MiB (ever '
                       f'{int(last["ever_pools"][i])} / {last["ever_mib"][i]:.1f} MiB)' for i, c in enumerate(CLASSES))
    print(f'SUMMARY mirror at frame {lines[-1][0]}: {mirror}; untracked {last["untracked_mib"][0]:.1f} MiB')
    print(f'SUMMARY dirty census cost: {avg("queries_per_frame", 0):.1f} write-watch queries, '
          f'{avg("scan_ms_per_frame", 0):.3f} ms per frame')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1560
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    fr, fps, mx, gpu, vram, ram, bound = None, [], [], [], [], [], {}
    frame_counts = []
    dirty, dframes, dlast = [], [], None
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        if line.startswith('run_game:'):
            print(line.rstrip())
        elif m := MEMCLASS.search(line):
            print(m[1])
        elif m := DIRTY.search(line):
            dlast = int(m[1])
            if lo < dlast <= hi:
                dirty.append((dlast, dirty_fields(m[2])))
        elif m := DIRTY_FRAMES.search(line):
            if dlast is not None and lo < dlast <= hi:
                dframes += [tuple(float(x) for x in t.split('/')) for t in m[1].split()]
        elif m := PERF.search(line):
            f = int(m[1])
            if lo < f <= hi:
                fps.append(float(m[4]))
                count = FRAME_COUNT.search(line)
                frame_counts.append(int(count[1]) if count else 60)   # assume full windows when older logs omit the count
                mx.append(float(m[3]))
                print(f'perf {f}: {m[4]} fps max {m[3]} pacing {m[5]}% pipe {m[6]}% tex {m[7]}%')
        elif m := GPU.search(line):
            f = int(m[1])
            if lo < f <= hi:
                if m[2]:
                    gpu.append(float(m[2]))
                if m[5]:
                    vram.append((float(m[5]), float(m[6])))
                ram.append((float(m[7]), float(m[8])))
                bound[m[9]] = bound.get(m[9], 0) + 1
                print(f'  gpu {f}: {m[2] or "n/a"} ms, fence wait render {m[3]} other {m[4]} ms, '
                      f'vram {m[5] or "n/a"} MiB, ram ws {m[7]} private {m[8]} MiB, bound {m[9]}')
        elif m := SCHED.search(line):
            fr = int(m[1])
            if lo < fr <= hi:
                print(f'  sched {fr}: {m[2]}')
        elif (m := THREAD.search(line)) and fr and lo < fr <= hi:
            print(f'    {m[1] or ""}{m[2]} {m[3]} run {m[4]} core {m[5]} host {m[6]} sleep {m[7]} sync {m[8]} cv {m[9]} '
                  f'lock {m[10]} addr {m[11]} cpu {m[12]} blk {m[13]} pre {m[14]}')
        elif (m := RWAIT.search(line)) and fr and lo < fr <= hi:
            print(f'    rw{m[1]}: {m[2]}')
    if fps:
        # Windows contain frames, not equal durations: total frames / total time is the weighted harmonic mean.
        mean_fps = statistics.harmonic_mean(fps, weights=frame_counts)
        print(f'SUMMARY frames {lo}-{hi}: mean fps {mean_fps:.1f}, min window fps {min(fps):.1f}, '
              f'max frame {max(mx):.1f} ms')
    if ram:
        g = f'gpu mean {statistics.mean(gpu):.2f} ms' if gpu else 'gpu n/a'
        v = f'vram max {max(u for u, _ in vram):.0f} of {vram[-1][1]:.0f} MiB' if vram else 'vram n/a'
        b = ', '.join(f'{k} {n}' for k, n in sorted(bound.items()))
        print(f'SUMMARY gpu: {g}, {v}, ram ws max {max(w for w, _ in ram):.0f} MiB, '
              f'private max {max(p for _, p in ram):.0f} MiB; bound windows: {b}')
    dirty_summary(dirty, dframes)


if __name__ == '__main__':
    main()
