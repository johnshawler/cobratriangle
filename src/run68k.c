/* run68k.c - Amiga: the translated code is the 68k back end of tools/recomp.py (src/gen68k.s), entered
 * through the trampolines in src/nes68k.s. */
#include "nes.h"
#include "run.h"
extern void *reset_fn, *nmi_fn;                 /* src/gen68k.s */
uint16_t asm_run(void *fn);                     /* src/nes68k.s */
uint16_t asm_dispatch(unsigned pc);
uint16_t run_reset(void) { return asm_run(reset_fn); }
uint16_t run_nmi(void) { return asm_dispatch((uint16_t)(nes_rd(0xfffa) | nes_rd(0xfffb) << 8)); }   /* the vector of the mapped bank */
uint16_t run_dispatch(uint16_t pc) { return asm_dispatch(pc); }
