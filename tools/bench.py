#!/usr/bin/env python3
"""
bench.py - Block 4 performance gate: is Butterscotch's VM even viable in soft-float
MIPS32 on the GB300V2?

Methodology (mirrors /home/Sajnaps/re3-sf2000/runner/gb300_runner.c, which does the
same thing for GTA III):

  1. Build the runner for the host twice, identically except for the float ABI:
       native : x86-64, SSE/AVX autovectorisation on  -> the fast reference
       gbsim  : -m32 -mfpmath=387 -mno-sse* -fno-tree-vectorize
     The gbsim build has 32-bit pointers and every float in memory instead of a vector
     register, which is the closest stand-in for the console available on x86.  It is
     still real silicon, so it models the console's *arithmetic*, not its clock.
  2. Run headless.  loop.c:1470 skips the frame limiter in headless mode, so the
     process free-runs and wall time / frames is the true per-frame cost with no
     sleeping.  --trace-frames prints a per-frame time we parse instead of guessing.
  3. Scale by a cpu-factor to account for the clock and microarchitecture gap between
     this host and a ~810-900 MHz JZ4775.  re3's runner defaults to 10 for soft-float
     (8 for hardware FPU); we report a sensitivity table because that factor is the
     least trustworthy number in the whole exercise.

The renderer is deliberately NOT in this measurement: the no-op renderer is compiled
in, so this is the VM + data.win cost alone, i.e. the floor.  Adding a software
rasteriser can only make it worse, which is exactly why the gate is evaluated here,
before the renderer is written.

Usage:
    bench.py [--frames 1200] [--cpu-factor 10] [--out report]
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATAWIN = os.path.join(ROOT, 'assets', 'game.unx')

# loop.c logs "Frame %d (End, %.2f ms)" when --trace-frames is on.
FRAME_RE = re.compile(r'Frame (\d+) \(End, ([0-9.]+) ms\)')

# Windows of Undertale's intro that exercise different things.  Labels are what the
# frames actually show, taken from docs/golden/.
WINDOWS = [
    (60,   160,  'intro narration + textbox (bitmap font, low overdraw)'),
    (360,  460,  'fading background (full-screen alpha blend)'),
    (960,  1060, 'Mt. Ebolt title card (large background, text)'),
]

# The console's budget.
TARGET_FPS = 30.0
BUDGET_MS = 1000.0 / TARGET_FPS
GATE_FPS = 8.0


def run(binary, frames, extra=()):
    """Run headless, return (per-frame ms list, wall seconds, load seconds)."""
    cmd = [binary, DATAWIN,
           '--headless',
           '--lazy-rooms', '--lazy-textures',
           '--load-type', 'load-per-chunk',
           '--exit-at-frame', str(frames),
           '--trace-frames']
    cmd.extend(extra)

    t0 = time.perf_counter()
    proc = subprocess.run(cmd, cwd=os.path.dirname(binary),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=3600)
    wall = time.perf_counter() - t0

    per_frame = {}
    load_ms = None
    for line in proc.stdout.splitlines():
        m = FRAME_RE.search(line)
        if m:
            per_frame[int(m.group(1))] = float(m.group(2))
        elif 'successfully!' in line and load_ms is None:
            pass
    return per_frame, wall, proc.stdout


def stats(values):
    if not values:
        return None
    s = sorted(values)
    return {
        'n': len(s),
        'mean': statistics.fmean(s),
        'median': statistics.median(s),
        'p95': s[min(len(s) - 1, int(len(s) * 0.95))],
        'max': s[-1],
    }


def fmt(st, factor):
    if st is None:
        return 'no frames'
    return ('mean %7.2f ms  median %7.2f  p95 %7.2f  max %8.2f  '
            '-> console %7.1f ms (%5.1f fps)'
            % (st['mean'], st['median'], st['p95'], st['max'],
               st['mean'] * factor, 1000.0 / (st['mean'] * factor)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frames', type=int, default=1100)
    ap.add_argument('--cpu-factor', type=float, default=10.0)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    variants = [
        ('native  x86-64 + SSE', os.path.join(ROOT, 'build', 'bench_native', 'butterscotch')),
        ('gbsim   i386 x87, no SSE', os.path.join(ROOT, 'build', 'bench_gbsim', 'butterscotch')),
    ]
    for label, path in variants:
        if not os.path.exists(path):
            print('missing %s (%s) - build it first' % (label, path))
            return 1

    report = []
    results = {}

    for label, path in variants:
        print('=' * 78)
        print(label)
        print('=' * 78, flush=True)
        per_frame, wall, raw = run(path, args.frames)
        if not per_frame:
            print('  no per-frame timings captured; tail of output:')
            print('\n'.join(raw.splitlines()[-15:]))
            return 1
        results[label] = per_frame
        report.append('%s  (%.1f s wall for %d frames)' % (label, wall, len(per_frame)))
        print()

    sim = 'gbsim   i386 x87, no SSE'
    ref = 'native  x86-64 + SSE'

    print()
    print('=' * 78)
    print('per-frame cost, host time (VM + data.win only, no rendering)')
    print('=' * 78)
    for label in (ref, sim):
        pf = results[label]
        allv = [pf[k] for k in sorted(pf)]
        print('%-28s whole run        %s' % (label, fmt(stats(allv), 1.0)))
        for lo, hi, desc in WINDOWS:
            vals = [pf[k] for k in range(lo, hi + 1) if k in pf]
            if vals:
                print('%-28s   f%-5d-%-5d %s' % (label, lo, hi, fmt(stats(vals), 1.0)))
        print()

    # soft-float penalty, measured rather than assumed
    ratio = (statistics.fmean([results[sim][k] for k in results[sim]]) /
             statistics.fmean([results[ref][k] for k in results[ref]]))
    print('measured soft-float + 32-bit penalty: %.2fx' % ratio)
    print()

    print('=' * 78)
    print('projected GB300V2 / SF2000 (JZ4775, MIPS32, no FPU)')
    print('=' * 78)
    print('%-10s %-14s %-14s %-14s %s' % ('cpu-fact', 'whole run', 'worst window', 'fps', 'verdict'))
    verdict = None
    for factor in (6.0, 8.0, 10.0, 12.0, 15.0):
        pf = results[sim]
        allv = [pf[k] for k in sorted(pf)]
        worst = None
        for lo, hi, _ in WINDOWS:
            vals = [pf[k] for k in range(lo, hi + 1) if k in pf]
            if vals:
                m = statistics.fmean(vals)
                worst = m if worst is None else max(worst, m)
        ms = statistics.fmean(allv) * factor
        fps = 1000.0 / ms
        v = 'PASS' if fps >= GATE_FPS else 'FAIL (below %.0f fps gate)' % GATE_FPS
        if factor == args.cpu_factor:
            verdict = (ms, fps, v)
        print('%-10.1f %7.1f ms     %7.1f ms     %6.1f      %s'
              % (factor, ms, worst * factor, fps, v))

    print()
    print('budget: %.1f ms/frame for %.0f fps; gate: %.0f fps with NO renderer yet'
          % (BUDGET_MS, TARGET_FPS, GATE_FPS))
    if verdict:
        print('at the default cpu-factor %.1f: %.1f ms/frame, %.1f fps -> %s'
              % (args.cpu_factor, verdict[0], verdict[1], verdict[2]))
    print()
    print('NOTE: this is the floor. The software renderer, texture uploads and the')
    print('      mixer all land on top of it. A software rasteriser at 320x240 with')
    print('      alpha blending is a fill-bound workload, and FroggyPE on real')
    print('      hardware managed 20-33 fps at that resolution with a 3D rasteriser.')

    if args.out:
        with open(args.out, 'w') as f:
            f.write('frames=%d\ncpu_factor=%.1f\nsoftfloat_penalty=%.3f\n'
                    % (args.frames, args.cpu_factor, ratio))
            for label, _ in variants:
                f.write('\n[%s]\n' % label)
                pf = results[label]
                st = stats([pf[k] for k in sorted(pf)])
                f.write('  mean_ms=%.3f median_ms=%.3f p95_ms=%.3f max_ms=%.3f\n'
                        % (st['mean'], st['median'], st['p95'], st['max']))
                for lo, hi, desc in WINDOWS:
                    vals = [pf[k] for k in range(lo, hi + 1) if k in pf]
                    if vals:
                        s2 = stats(vals)
                        f.write('  window_%d_%d mean_ms=%.3f  %s\n'
                                % (lo, hi, s2['mean'], desc))
        print('\nwrote %s' % args.out)

    return 0


if __name__ == '__main__':
    sys.exit(main())
