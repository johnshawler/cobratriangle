# Learnings from porting Cobra Triangle (NES, PAL, AxROM) to the A1200

The second NES port with this template (the first, Little League Baseball, is in history/llb/). General lessons
are folded into PLAYBOOK-NES.md; numbers are in REPORT.txt; brief problems in NOTES.md.

## 1. What was new against the first port, and how it was done

- **AxROM (mapper 7)**: four 32 KB banks switched as a whole, no fixed bank, every bank has its own vectors.
  The translator's bank model got a second mode (`AX` in tools/recomp.py): a store to $8000-$FFFF ends the
  translated function and the next address is dispatched in the bank then mapped. Continuation entries come
  from `LDA #n` before the store, from code that is identical in all banks (the $FF80 trampoline), and from
  new `B` lines in `nesref -c` traces (the first instruction run in a new bank). After a JSR to a function that
  may switch banks the caller checks `a3` and dispatches if its bank is gone. On the Amiga the bank register
  is a fast path (`.ax` in src/nes68k.s): a3/a4/nes_bank and the one-screen page, no C call.
- **NMI-driven frame model**: the main thread is `JMP *`; the NMI does the whole frame and resets the stack.
  A jump or branch to itself is an `IDLE` wait: `nes_idle()` longjmps back to `nes_main()`, which pushes the
  idle address and P and runs the NMI vector *of the mapped bank* (they differ per bank). verify.c does the same.
  The 6502 stack stays exact (the checksum covers it).
- **CHR RAM**: `nes_chr_ram` plus a dirty-tile list, written by the `$2007` fast path; the display redraws only
  cells and sprite clusters using a rewritten tile (a "tile seen on screen" table avoids scanning the
  nametable for tiles that are not shown; a burst of more than 12 tiles is a new tile set: redraw all).
- **PAL**: `nesref` got 312-line timing; one NES frame = one Amiga frame (TICKS_PER_SECOND 50).
- **A split that changes scroll and page**: the status bar. Kept as a bitmap (rebuilt when its cells or tiles
  change) and blitted into the scrolling buffer under the 180-line playfield view whenever the view moves; the
  row copies it covers are marked stale and redrawn when they scroll into view. Its backdrop (black) differs
  from the playfield's, so tiles are drawn with the attribute planes masked by the pixels: colour 0 stays 0
  and colour 4 is free for the bar's backdrop.
- **Sprites**: seven channels instead of four: a cluster in one NES palette p goes alone into sprite 2p or
  2p+1 (the Amiga sprite colour groups are the NES palettes), mixed clusters into attached pairs. Dropped
  clusters over the bench: 1,233 -> 214.

## 2. Bugs worth remembering

- `_io_wr_d2` (STY to I/O) fell through into `_set_prg` in the shared runtime: the first game never stored Y to
  an I/O address, so its checksums were equal. Cobra's `STY $8000` hung the Amiga after the title. Found by
  bisecting the tick where the Amiga stopped finishing and reading the helpers the new code paths used.
  Lesson: shared helpers need a test of every entry, not only what one game exercised.
- `LDA #$40; BIT $2002; BEQ *-3` is a sprite-0 wait; the wait detector took it for a vblank wait.
- Two emulator runs building the same program name at once corrupt each other (debug_run.sh/shot_at.sh now use
  NAME from port.conf; still run them one at a time).
- `tools/bench.sh` gives a run frames/25 + 60 seconds: a port slower than ~20 fps over 3,000 ticks shows
  "NO bench.txt" although it is only slow. Try a shorter run before suspecting a crash.

## 3. Numbers (bench ticks 1500-3000, play)

| Measure | Stock | 4 MB fast RAM |
| --- | --- | --- |
| 6502 instructions per frame (reference) | 7,450 avg, 8,700 max | same |
| Logic per tick, first build | 96k colour clocks | 58k |
| Logic per tick, after I/O fast paths and the CHR upload by hand | not measured separately | 53k |
| Display per frame, first build | 133k | 98k (sprites 41k, grouping 15k) |
| Display per frame, after the display fixes | not measured | 80k |
| Shown fps (tools/port.sh) | 11.5 -> 13.8 | 17.4 -> 21.9 |

What bought speed: PPU register and `$2007`/`$4017` read fast paths (about 100 C calls a tick gone): 17.4 ->
19.1 fps on fast RAM; the boat's CHR upload ($FFA1/$FFA9 in bank 2, ~160 `$2007` writes a tick) by hand: 19.1 ->
21.9; display: no nametable scan for unseen tiles, the bar re-rendered only when its own tiles change,
sprite cache invalidated per tile instead of entirely.

## 4. What to try next (fast RAM, play)

1. Sprites: 51 visible NES sprites a frame; clustering and rendering in C cost ~40k. Grouping in assembly and
   keeping clusters across frames (the scenery moves with the scroll) are the next big display item.
2. The straddling tile row under the bar is redrawn every frame the view moves (~40 tiles): draw only the lines
   above the bar, or move the bar blit to the tile boundary.
3. Logic: profile again restricted to play; `(zp),y` reads through `_rd_ea` (the decompressor at bank 1 $8430
   streams tiles through PHA into the stack page) and `_plp` are next.
4. Colour 0 is the playfield backdrop, so the overscan border is blue; use another index for the playfield
   backdrop and keep colour 0 black.
5. Measure a 68030 (FS-UAE accelerator options): the honest "cheapest upgrade" number.
