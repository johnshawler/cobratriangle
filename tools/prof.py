#!/usr/bin/env python3
"""prof.py <debug-run dir> [first_tick last_tick] - name the samples of the 1 kHz profiler (src/prof68k.s).

<dir> is what tools/debug_run.sh wrote: prof.bin (runtime address of the reset function, of game_tick, then one
program counter per millisecond), frames.bin (vertical blank, tick) and the debug program <NAME>_dbg.
Translated code is named from the symbols of src/gen68k.s (assembled again here), C code from the linked
program's symbols relative to _game_tick. With a tick range, only samples taken while those ticks ran count
(the busiest scene: PLAYBOOK-NES.md section 4)."""
import sys, struct, subprocess, bisect, collections, os, glob
d = sys.argv[1]
lo, hi = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (0, 1 << 30)
raw = open(os.path.join(d, 'prof.bin'), 'rb').read()
if raw[:4] == b'PRO3':                  # reset function, game_tick, tile8 (the display's assembly hunk)
    base, cbase, vbase = struct.unpack('>III', raw[4:16]); raw = raw[16:]
else:
    base, cbase = struct.unpack('>II', raw[:8]); vbase = None; raw = raw[8:]
pcs = struct.unpack('>%dI' % (len(raw) // 4), raw)
exe = glob.glob(os.path.join(d, '*_dbg'))[0]
def nm(args):
    out = subprocess.run(['docker', 'run', '--rm', '-v', os.getcwd() + ':/work', '-w', '/work', 'amigadev/crosstools:m68k-amigaos',
                          'bash', '-c', args], capture_output=True, text=True).stdout
    syms = []
    for l in out.split('\n'):
        p = l.split()
        if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
    return sorted(syms)
gen = nm('vasmm68k_mot -quiet -Fhunk -m68020 -I. -Isrc -o /tmp/g.o src/gen68k.s && m68k-amigaos-nm /tmp/g.o')
gnames = set(n for a, n in gen)
cs = [(a, n) for a, n in nm('m68k-amigaos-nm ' + exe) if n not in gnames and not n.startswith('F') and not n.startswith('D_')]
goff = dict((n, a) for a, n in gen)
gbase = base - goff.get('F0_' + '%04x' % 0, goff[min(goff, key=lambda k: goff[k])]) if False else None
ref = [n for a, n in gen if n.startswith('F')]
# the reset function's runtime address is base; its symbol is the one _reset_fn points at
rsym = subprocess.run(['grep', '-m1', '_reset_fn:', 'src/gen68k.s'], capture_output=True, text=True).stdout.split()[-1]
gbase = base - goff[rsym]
vid = nm('vasmm68k_mot -quiet -Fhunk -m68020 -I. -Isrc -o /tmp/v.o src/video68k.s && m68k-amigaos-nm /tmp/v.o') if vbase else []
voff = dict((n, a) for a, n in vid)
vaddr = [a + vbase - voff.get('_tile8', 0) for a, n in vid] if vbase else []; vname = [n for a, n in vid]
cmap = dict((n, a) for a, n in cs)
cb = cbase - cmap.get('_game_tick', 0)
gaddr = [a + gbase for a, n in gen]; gname = [n for a, n in gen]
caddr = [a + cb for a, n in cs]; cname = [n for a, n in cs]
# select samples by tick: frames.bin gives (vblank, tick) per drawn frame; samples are 1 ms apart, 20 per vblank
fr = open(os.path.join(d, 'frames.bin'), 'rb').read(); fr = struct.unpack('>%dI' % (len(fr) // 4), fr)
pairs = [(fr[i], fr[i + 1]) for i in range(0, len(fr), 2)]
v0 = pairs[0][0] if pairs else 0
def tick_at(sample):
    vb = v0 + sample // 20
    i = bisect.bisect_right([p[0] for p in pairs], vb) - 1
    return pairs[i][1] if i >= 0 else 0
cnt = collections.Counter(); n = 0
for i, pc in enumerate(pcs):
    if not (lo <= tick_at(i) < hi): continue
    n += 1
    j = bisect.bisect_right(gaddr, pc) - 1
    if j >= 0 and pc - gaddr[j] < 0x4000 and gname[j] and pc < gaddr[-1] + 0x2000: cnt['gen ' + gname[j]] += 1; continue
    if vaddr:
        j = bisect.bisect_right(vaddr, pc) - 1
        if j >= 0 and pc - vaddr[j] < 0x1000: cnt['asm ' + vname[j]] += 1; continue
    j = bisect.bisect_right(caddr, pc) - 1
    cnt['C ' + (cname[j] if j >= 0 else '?')] += 1
print('samples', n)
for k, v in cnt.most_common(40): print('%5.1f%% %s' % (100.0 * v / max(n, 1), k))
