#!/usr/bin/env python3
"""cmp_trace.py trace.bin N - compare the Amiga's per-tick checksums (tools/debug_run.sh) with the host test's."""
import sys, struct, subprocess
tr = open(sys.argv[1], 'rb').read(); n = len(tr) // 4
am = struct.unpack('>%dI' % n, tr)
out = subprocess.run(['out/host_test', sys.argv[2], '-c'], capture_output=True, text=True).stdout.split()
host = [int(x, 16) for x in out[:n]]
for i, (a, h) in enumerate(zip(am, host)):
    if a != h: print('first difference at tick', i + 1, 'amiga %08x host %08x' % (a, h)); break
else: print('equal for', min(len(am), len(host)), 'ticks')
