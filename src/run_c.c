/* run_c.c - host test: the translated code is the C back end of tools/recomp.py (src/gen_*.c). */
#include "nes.h"
#include "run.h"
#include "gen_code.h"
uint16_t run_reset(void) { return nes_reset_entry(); }
uint16_t run_nmi(void) { return nes_dispatch((uint16_t)(nes_rd(0xfffa) | nes_rd(0xfffb) << 8)); }   /* the vector of the mapped bank */
uint16_t run_dispatch(uint16_t pc) { return nes_dispatch(pc); }
