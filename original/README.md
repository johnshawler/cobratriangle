# The game to port

**Cobra Triangle** (Rare / Nintendo, 1989), the European (PAL) release for the NES. The ROM is
`original/cobra.nes`; it is not in the repository (copyrighted; `.gitignore` keeps it and everything made from it out).

- Mapper 7 (AxROM): 128 KB PRG as four 32 KB banks switched as a whole by a write to $8000-$FFFF, bit 4 of the
  same register picks one of two one-screen nametables. 8 KB CHR RAM (no CHR ROM: the game writes its tiles).
  NES 2.0 header says PAL: 50 frames a second, so one NES frame = one Amiga frame (no 60 to 50 Hz rescale).
- Every bank has its own reset/NMI vectors. Common code at $FF80-$FFB6 is the same in all banks: a far call
  trampoline ($FF80: switch, JSR $8000, switch back), bank switch-and-return ($FF9C), a delay loop ($FFB0).
- NMI-driven: after start-up the main thread is a `JMP *` loop ($F73E, $A5E0, $A569, $CFF6). The NMI handler
  runs the whole frame and ends in such a loop again with the stack reset (`LDX #$FD/$FF; TXS`).
- Raster split: a fixed status bar under a 180-line playfield. In the NMI: sprite 0 at y=$B2 and a wait for its
  hit (`LDA #$40; BIT $2002; BEQ`), then cycle-timed delay loops ($FFB0, $FD59) before the status bar's
  scroll, one-screen page (bit 4 of the bank register) and PPUMASK writes at lines 180-194.
- The playfield scrolls diagonally (both axes) in one 32x30 nametable; ~10 CHR RAM tiles rewritten per frame.
- Self-playing mode: none (title, high scores, title again). The bench presses START at ticks 480 and 600 and
  then steers and fires by script (`game_bench_input` in `src/logic.c`); play starts at tick ~740.
- Controls: D-pad steers, A fires, B turbo, START pause.
- Load (6502 instructions per frame outside wait and delay loops, `tools/nesref` with `WAITPCS`):
  title/high scores 500-1,900 (spikes to 11k at screen changes, with rendering off);
  play (bench ticks 750-3000): average 7,450, median 7,700, 90th percentile 8,400, max 8,700.

## Feasibility note (milestones 2-3 of PIPELINE.md, written before the Amiga code)

- Class and route: 6502 binary, translated once on the host (`tools/recomp.py`), 68k back end on the Amiga.
- Logic: 7,450-8,700 6502 instructions per 50 Hz frame. At the playbook's 10-13 colour clocks each from chip RAM
  that is 75k-113k colour clocks: more than the whole 71k frame. **A stock A1200 cannot hold 50 Hz; expect
  roughly 25-35 fps shown, logic at 50 ticks.** With 4 MB fast RAM (5-6 colour clocks each): 37k-52k for logic,
  against 57k at the 80% gate, leaving 5-20k for the display: 50 Hz only with hand-written hot routines
  and a cheap display. Cheapest upgrade to name: fast RAM; a 68030 accelerator is the safe one.
- Display: 4 bitplanes (16 NES background colours), hardware scroll in both axes over an unwrapped world, CHR RAM
  tiles converted at run time when the game rewrites them, NES sprites on hardware sprites (fetch 0: sprites 0-6).
  The status bar cannot be a raster split (the kit has none): it is drawn into the scrolling buffer under the
  180-line playfield view each frame the scroll moves.
- Cut from the original: the cycle-timed raster effects (their timing, not their result), lag frames (the
  translated frame always finishes its logic; the NES lags ~1 frame in 750 in play), DMC samples.
