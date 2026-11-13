#!/usr/bin/env python3
"""framestat.py <debug-run dir> [first_tick] - per drawn frame: ticks run, logic per tick, display time
(tools/debug_run.sh writes frames.bin, framevt.bin, ticktime.bin). The question it answers: does one tick plus
one picture fit in a frame (71,051 colour clocks)?"""
import struct, sys, collections
D = sys.argv[1] + '/'; first = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
u = lambda f: (lambda b: struct.unpack('>%dI' % (len(b) // 4), b))(open(D + f, 'rb').read())
fr = u('frames.bin'); pairs = [(fr[i], fr[i + 1]) for i in range(0, len(fr), 2)]
vt = u('framevt.bin'); tt = u('ticktime.bin')
rows = []
for i in range(1, min(len(pairs), len(vt))):
    t0, t1 = pairs[i - 1][1], pairs[i][1]
    if t0 < first or t1 <= t0: continue
    rows.append((t1 - t0, sum(tt[t0:t1]) // (t1 - t0), vt[i]))
q = lambda xs, p: sorted(xs)[int(len(xs) * p)]
L = [r[1] for r in rows]; V = [r[2] for r in rows]; S = [r[1] + r[2] for r in rows]
print('frames', len(rows), 'ticks per frame', sorted(collections.Counter(r[0] for r in rows).items()))
for name, xs in (('logic/tick', L), ('display', V), ('one tick + display', S)):
    print('%-18s avg %6d  p10 %6d  p50 %6d  p90 %6d' % (name, sum(xs) // len(xs), q(xs, .1), q(xs, .5), q(xs, .9)))
print('one tick + display under 71051: %d%% of frames' % (100 * sum(1 for x in S if x < 71051) // len(S)))
