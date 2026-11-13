# Notes: where the brief was unclear, wrong or missing something

(Earlier ports: history/*/NOTES.md. Add one line for every new place.)
- The brief and kit assume one display set-up for the whole picture; a NES status bar under a raster split (scroll
  and nametable change at line 180) has no kit call. Worked around by blitting the bar into the scrolling buffer
  under the view whenever the view moves (src/video.c); a kit split-screen call would be cheaper and exact.
- tools/setup.sh installed m68k-amigaos-gcc 16.2.1b (AmigaDev image of 2026-10), not the 6.5.0b the brief names.
- A PAL NES game needs TICKS_PER_SECOND 50 (src/main.c); the brief's 60 Hz advice does not apply.
- The brief's Step 1 route for 6502 code does not mention bank switching that swaps all of $8000-$FFFF under the
  running code (AxROM): the translator must end a function at the bank write and dispatch the rest.
- tools/debug_run.sh and tools/shot_at.sh had the previous game's program name built in; they now use NAME from port.conf.
- tools/bench.sh boots AROS only; the real Kickstart is reached by passing --kickstart_file=<rom> as an extra option (worked unchanged).
