/* video.c - shows the NES picture on the AGA display (Amiga only; tools/vhost.c runs it on the host). Reads the
 * PPU state the game logic left in src/nes.c once per frame and changes only what changed. Written for Cobra
 * Triangle (AxROM: one-screen nametables, CHR RAM, a status bar under a sprite-0 split):
 *
 *  - Playfield: 4 bitplanes = the NES's 16 background colours (attribute bits are planes 2 and 3, set only under
 *    non-zero pixels, so colour-0 pixels are colour 0). Hardware scrolling in both axes over an unwrapped
 *    world: the one-screen nametable is a 256 x 240 torus; tile columns are drawn into the buffer as the camera
 *    reaches them, and every tile row is kept at y and y + 240, so the vertical wrap is a plain scroll position.
 *  - CHR RAM: tiles are read from the game's own pattern tables (nes_chr_ram); when the game rewrites a tile,
 *    the cells that show it are drawn again (and the sprite lists rebuilt).
 *  - Status bar: the NES shows another nametable page below line 180 (sprite-0 hit, then a timed scroll and
 *    page change). The kit has no raster split, so the bar is kept as a bitmap and blitted into the scrolling
 *    buffer right under the playfield view whenever the view moves; the rows it covered are drawn again when
 *    they come into view. Colour 0 of the bar (its backdrop, black) is drawn as colour 4, which nothing else
 *    uses, because the playfield's backdrop colour 0 is different.
 *  - Sprites: NES sprites are grouped into clusters at most 64 pixels wide and drawn into 64-pixel hardware
 *    sprites, three attached pairs (15 colours) and sprite 6 for clusters in NES sprite palette 3, each reused
 *    down the screen (several control words in one sprite list).
 */
#include <proto/exec.h>
#include <exec/memory.h>
#include <hardware/custom.h>
#include <string.h>
#include "sys.h"
#include "disp.h"
#include "nes.h"
#include "video.h"
#ifndef __m68k__
#include <stdio.h>
#include <stdlib.h>
#endif

#define PLANES   4
#define LEVEL_W  8192                   /* unwrapped world width: the camera is re-centred if it gets near an end */
#define VIEW_X   32                     /* the 256-pixel NES picture starts 32 pixels into the 320-pixel view */
#define SPR_W    64
#define BUF_H    480                    /* every tile row twice: y and y + 240 */

/* The status bar, as Cobra Triangle times it (tools/nesref trace of a play frame): sprite-0 hit at line 180,
   rendering off (backdrop black) until line 193, the bar page's nametable from v = $2000 at line 190. */
#define BAR_V_LINE   190                /* NES line that shows pixel row 0 of the bar page */
#define BAR_SHOW     194                /* first NES line with the bar's picture (before it: backdrop) */
#define BAR_LINES    64                 /* bitmap lines (bar lines are pf_h..239: at most 60) */
#define BAR_ROWB     64                 /* bytes a bitmap line */
#define BAR_WORDS    20                 /* blitted words a line: 2 border words, 16 for 256 pixels, 2 border */

#ifdef __m68k__
#define GETW(p)    (*(UWORD *)(p))
#define PUTW(p, v) (*(UWORD *)(p) = (UWORD)(v))
#else
#define GETW(p)    ((UWORD)(((UBYTE *)(p))[0] << 8 | ((UBYTE *)(p))[1]))
#define PUTW(p, v) (((UBYTE *)(p))[0] = (UBYTE)((v) >> 8), ((UBYTE *)(p))[1] = (UBYTE)(v))
#endif

#ifdef LOGIC_TRACE
ULONG vprof[12], vtiles;                 /* debug: colour clocks in cell redraws, streaming, sprites; tiles drawn */
extern uint32_t logic_frames;
#ifdef NOVP                             /* debug builds without the inner timers (their own cost is large) */
#define VP(i, x) do { x; } while (0)
#else
#define VP(i, x) do { ULONG t0_ = sys_now(); x; if (logic_frames >= 2000 && logic_frames < 2500) vprof[i] += sys_now() - t0_; } while (0)
#endif
#else
#define VP(i, x) do { x; } while (0)
#endif

const DispCfg video_display = { PLANES, 1, 0, 240, BUF_H, LEVEL_W, 1, 0, SPR_W, 1 };

static const ULONG nes_rgb[64] = {
0x666666,0x002a88,0x1412a7,0x3b00a4,0x5c007e,0x6e0040,0x6c0600,0x561d00,0x333500,0x0b4800,0x005200,0x004f08,0x00404d,0,0,0,
0xadadad,0x155fd9,0x4240ff,0x7527fe,0xa01acc,0xb71e7b,0xb53120,0x994e00,0x6b6d00,0x388700,0x0c9300,0x008f32,0x007c8d,0,0,0,
0xfffeff,0x64b0ff,0x9290ff,0xc676ff,0xf36aff,0xfe6ecc,0xfe8170,0xea9e22,0xbcbe00,0x88d800,0x5ce430,0x45e082,0x48cdde,0x4f4f4f,0,0,
0xfffeff,0xc0dfff,0xd3d2ff,0xe8c8ff,0xfbc2ff,0xfec4ea,0xfeccc5,0xf7d8a5,0xe4e594,0xcfef96,0xbdf4ab,0xb3f3cc,0xb5ebf2,0xb8b8b8,0,0};

UBYTE video_flip[256];                  /* bit-reversed bytes, for h-flipped sprites */
ULONG video_plane;                      /* bytes from one bitplane to the next (src/video68k.s) */
#ifdef __m68k__
void tile8(UBYTE *d __asm("a0"), const UBYTE *chr __asm("a1"), ULONG attr __asm("d0"));
void spr_put(UBYTE *e __asm("a0"), UBYTE *o __asm("a1"), const UBYTE *chr __asm("a2"), ULONG ctl __asm("d0"));
#else                                   /* the same in C, for the host harness tools/vhost.c */
static void tile8(UBYTE *d, const UBYTE *chr, ULONG attr)
{
    int i;
    for (i = 0; i < 8; i++, d += 64) {
        UBYTE m = (UBYTE)(chr[i] | chr[i + 8]);
        d[0] = chr[i]; d[video_plane] = chr[i + 8]; d[2 * video_plane] = (UBYTE)(attr & m); d[3 * video_plane] = (UBYTE)((attr >> 8) & m);
    }
}
static void spr_put(UBYTE *e, UBYTE *o, const UBYTE *chr, ULONG ctl)
{
    int dx = ctl & 63, sh = 8 - (dx & 7), r;
    e += dx >> 3; o += dx >> 3;
    for (r = 0; r < 8; r++, e += 16, o += 16) {
        int row = (ctl & 0x200) ? 7 - r : r;
        UBYTE p0 = chr[row], p1 = chr[row + 8];
        UWORD m, k;
        if (ctl & 0x100) { p0 = video_flip[p0]; p1 = video_flip[p1]; }
        if (!(p0 | p1)) continue;
        m = (UWORD)((p0 | p1) << sh); k = (UWORD)~m;
        PUTW(e, (GETW(e) & k) | (p0 << sh)); PUTW(e + 8, (GETW(e + 8) & k) | (p1 << sh));
        if (ctl & 0x1000) { PUTW(o, (GETW(o) & k) | ((ctl & 0x400) ? m : 0)); PUTW(o + 8, (GETW(o + 8) & k) | ((ctl & 0x800) ? m : 0)); }
    }
}
#endif

/* ---------------------------------------------------------------- playfield */
static LONG cam_ux;                     /* unwrapped world x of NES screen x = 0 */
static WORD sx_prev = -1;
static LONG col_lo, col_hi;             /* unwrapped tile columns col_lo..col_hi are drawn (col_lo > col_hi: none) */
static LONG work_col; static WORD work_row = -1;
static UWORD pf_page;                   /* nes_vram offset of the playfield's one-screen page: 0 or $400 */
static UBYTE bgsel;                     /* background pattern table: 0 = $0000, 1 = $1000 */
static UBYTE drawn_bgsel = 0xff; static UWORD drawn_page = 0xffff;
static int pf_h = 240;                  /* playfield lines: the split line, or 240 */
static int cur_sy;                      /* the vertical scroll being drawn for */
static UBYTE mask_left;                 /* PPUMASK of the playfield: bit 1 clear hides the left 8 pixels */

static const UBYTE *bg_chr(UBYTE tile) { return nes_chr + (bgsel ? 0x1000 : 0) + tile * 16; }
static UWORD nt_cell(LONG uc, int r) { return (UWORD)(pf_page | (r << 5) | (uc & 31)); }

/* Columns outside the 256-pixel NES picture are drawn in colour 4 (black, like the NES's border): their
   nametable content belongs to the other side of the wrap. */
static UBYTE colstate[64];              /* by unwrapped column & 63: 1 drawn from the nametable, 2 drawn blank */
static int col_visible(LONG uc) { LONG x = uc * 8; return x + 8 > cam_ux + ((mask_left & 2) ? 0 : 8) && x < cam_ux + 256; }

/* rowlines[r][c]: how many lines (from the top) of copy c of tile row r are up to date: the copy at y = 8r
   (c = 0) or at y = 8r + 240 (c = 1). 8 = all; fewer when the status bar was blitted over its lower part. */
static UBYTE rowlines[30][2];
static UBYTE tused[256];                /* background tiles drawn since the last full redraw (CHR RAM changes) */
static int need_lines(int r, int c, int sy)
{
    int yc = 8 * r + 240 * c, n = sy + pf_h - yc;
    if (yc + 8 <= sy || n <= 0) return 0;
    return n > 8 ? 8 : n;
}

/* lines f..t-1 of a tile, attribute planes only under its pixels (as tile8), or of a blank (border) tile */
static void tile_part(UBYTE *d, const UBYTE *chr, ULONG attr, int f, int t)
{
    for (d += f * 64; f < t; f++, d += 64) {
        UBYTE m = (UBYTE)(chr[f] | chr[f + 8]);
        d[0] = chr[f]; d[video_plane] = chr[f + 8]; d[2 * video_plane] = (UBYTE)(attr & m); d[3 * video_plane] = (UBYTE)((attr >> 8) & m);
    }
}
static const UBYTE blank_tile[16];

/* draw lines f0..t0-1 of copy 0 and f1..t1-1 of copy 1 of the tile at column uc, row r */
static void draw_rows(LONG uc, int r, int f0, int t0, int f1, int t1)
{
    UWORD cell = nt_cell(uc, r);
    UBYTE tile = nes_vram[cell], tx = (UBYTE)(uc & 31);
    UBYTE attr = nes_vram[pf_page | 0x3c0 | ((r >> 2) << 3) | (tx >> 2)];
    UBYTE pal = (UBYTE)((attr >> (((r & 2) << 1) | (tx & 2))) & 3);
    ULONG at = (ULONG)(((pal & 2) ? 0xff00 : 0) | ((pal & 1) ? 0xff : 0));
    const UBYTE *chr;
    LONG x = uc * 8;
    UBYTE *d;
    if (x < -64 || x >= LEVEL_W + 320 || (f0 >= t0 && f1 >= t1)) return;
#ifdef LOGIC_TRACE
    { extern ULONG vtiles; vtiles++; }
#endif
    d = disp_front + (ULONG)(r * 8) * 64 + ((x + disp_margin) >> 3);
    if (colstate[uc & 63] == 2) { chr = blank_tile; at = 0xff; }   /* border: colour 4 (blank_tile has no pixels: */
    else { chr = bg_chr(tile); tused[tile] = 1; }                   /*  the mask below is forced for it)          */
    if (chr == blank_tile) {
        int i;
        for (i = f0; i < t0; i++) { UBYTE *q = d + i * 64; q[0] = 0; q[video_plane] = 0; q[2 * video_plane] = 0xff; q[3 * video_plane] = 0; }
        for (i = f1; i < t1; i++) { UBYTE *q = d + 240 * 64 + i * 64; q[0] = 0; q[video_plane] = 0; q[2 * video_plane] = 0xff; q[3 * video_plane] = 0; }
        return;
    }
    if (f0 == 0 && t0 == 8) tile8(d, chr, at); else if (f0 < t0) tile_part(d, chr, at, f0, t0);
    if (f1 == 0 && t1 == 8) tile8(d + 240 * 64, chr, at); else if (f1 < t1) tile_part(d + 240 * 64, chr, at, f1, t1);
}
static void draw_tile(LONG uc, int r) { draw_rows(uc, r, 0, rowlines[r][0], 0, rowlines[r][1]); }

static void want_cols(LONG *lo, LONG *hi)
{
    *lo = (cam_ux - VIEW_X - 8) >> 3;
    *hi = (cam_ux - VIEW_X + 320 + 8) >> 3;
}

static void invalidate(void) { col_lo = 1; col_hi = 0; work_row = -1; memset(tused, 0, sizeof tused); }

/* make every row's copies have the lines the scroll position sy shows */
static void sync_rows(int sy)
{
    int r;
    LONG uc;
    for (r = 0; r < 30; r++) {
        int n0 = need_lines(r, 0, sy), n1 = need_lines(r, 1, sy), h0 = rowlines[r][0], h1 = rowlines[r][1];
        if (!n0) h0 = 0;                                /* not shown: forgotten (drawn again in full when needed) */
        if (!n1) h1 = 0;
        if ((n0 > h0 || n1 > h1) && col_lo <= col_hi) {
            int f0 = h0, t0 = n0 > h0 ? n0 : h0, f1 = h1, t1 = n1 > h1 ? n1 : h1;
            if (n0 <= h0) f0 = t0;
            if (n1 <= h1) f1 = t1;
            for (uc = col_lo; uc <= col_hi; uc++) draw_rows(uc, r, f0, t0, f1, t1);
            if (work_row > r) draw_rows(work_col, r, f0, t0, f1, t1);
        }
        if (n0 > h0) h0 = n0;
        if (n1 > h1) h1 = n1;
        rowlines[r][0] = (UBYTE)h0; rowlines[r][1] = (UBYTE)h1;
    }
}

/* draw up to n tiles of the columns that are needed but not drawn yet */
static void stream_cols(int n)
{
    LONG lo, hi;
    want_cols(&lo, &hi);
    if (col_lo > col_hi) { col_lo = (lo + hi) / 2; col_hi = col_lo - 1; work_row = -1; }
    if (col_lo < lo - 2) col_lo = lo - 2;                 /* forget columns far behind */
    if (col_hi > hi + 2) col_hi = hi + 2;
    while (n > 0) {
        if (work_row < 0) {
            if (col_hi < hi) work_col = col_hi + 1;
            else if (col_lo > lo) work_col = col_lo - 1;
            else return;
            work_row = 0;
            colstate[work_col & 63] = col_visible(work_col) ? 1 : 2;
        }
        if (rowlines[work_row][0] | rowlines[work_row][1]) { draw_tile(work_col, work_row); n--; }
        if (++work_row == 30) {
            if (work_col > col_hi) col_hi = work_col; else col_lo = work_col;
            work_row = -1;
        }
    }
}

/* columns that crossed the edge of the NES picture since they were drawn */
static LONG edges_cam = -99999;
static void fix_edges(void)
{
    LONG uc, lo = col_lo, hi = col_hi;
    int r, step = 1;
    /* a column's state only changes near the picture's edges, unless the camera jumped */
    if (cam_ux - edges_cam < 8 && edges_cam - cam_ux < 8) step = 0;
    edges_cam = cam_ux;
    for (uc = lo; uc <= hi; uc++) {
        if (!step && uc > (cam_ux + 16) >> 3 && uc < (cam_ux + 256 - 16) >> 3) { uc = ((cam_ux + 256 - 16) >> 3) - 1; continue; }
        UBYTE want = col_visible(uc) ? 1 : 2;
        if (colstate[uc & 63] != want) {
            colstate[uc & 63] = want;
            for (r = 0; r < 30; r++) draw_tile(uc, r);
        }
    }
}

/* the NES changed playfield nametable cell i: redraw it where it is on screen (or the 16 tiles of an attribute cell) */
static void redraw_cell(UWORD i)
{
    LONG uc;
    int r, c;
    if ((i & 0x3ff) < 0x3c0) {
        int cx = i & 31, row = (i >> 5) & 31;
        if (row >= 30) return;
        for (uc = col_lo + ((cx - col_lo) & 31); uc <= col_hi; uc += 32) draw_tile(uc, row);
        if (work_row > row && (work_col & 31) == cx) draw_tile(work_col, row);
    } else {
        int a = i & 0x3f, ax = (a & 7) * 4, ay = (a >> 3) * 4;
        for (c = 0; c < 4; c++) {
            for (uc = col_lo + ((ax + c - col_lo) & 31); uc <= col_hi; uc += 32)
                for (r = ay; r < ay + 4 && r < 30; r++) draw_tile(uc, r);
            if ((work_col & 31) == ax + c)
                for (r = ay; r < ay + 4 && r < 30 && r < work_row; r++) draw_tile(work_col, r);
        }
    }
}

/* the game rewrote background tile t (0..255 in the background pattern table): redraw the cells showing it */
static void redraw_tile(UBYTE t)
{
    int i;
    const UBYTE *v = nes_vram + pf_page;
    if (!tused[t]) return;                              /* not on screen: nothing to redraw */
    for (i = 0; i < 960; i++) if (v[i] == t) redraw_cell((UWORD)(pf_page | i));
}

/* ---------------------------------------------------------------- status bar */
static UBYTE *bar_bm;                   /* chip RAM: 4 planes x BAR_LINES x BAR_ROWB; line k = NES line pf_h + k */
#define BAR_PLANE ((ULONG)BAR_LINES * BAR_ROWB)
static UWORD bar_page;
static UBYTE bar_tused[256];            /* tiles the bar bitmap shows */
static int bar_valid, bar_drawn_h;
static LONG bar_x = -1; static int bar_y = -1, bar_h;

/* one cell of the bar page into the bar bitmap (2 border words each side, colour 0 drawn as colour 4) */
static void bar_cell(int r, int c)
{
    int k, top = pf_h;
    UWORD cell = (UWORD)(bar_page | (r << 5) | c);
    UBYTE attr = nes_vram[bar_page | 0x3c0 | ((r >> 2) << 3) | (c >> 2)];
    UBYTE pal = (UBYTE)((attr >> (((r & 2) << 1) | (c & 2))) & 3);
    const UBYTE *chr = bg_chr(nes_vram[cell]);
    UBYTE *b;
    bar_tused[nes_vram[cell]] = 1;
    for (k = 0; k < 8; k++) {
        int line = BAR_V_LINE + r * 8 + k - top;          /* bitmap line of NES line BAR_V_LINE + 8r + k */
        UBYTE p0 = chr[k], p1 = chr[k + 8], m = (UBYTE)(p0 | p1);
        if (line + top < BAR_SHOW || line < 0 || line >= BAR_LINES) continue;
        b = bar_bm + line * BAR_ROWB + 4 + c;
        b[0] = p0; b[BAR_PLANE] = p1;
        b[2 * BAR_PLANE] = (pal & 1) ? 0xff : (UBYTE)~m;
        b[3 * BAR_PLANE] = (pal & 2) ? m : 0;
    }
}

static void render_bar(void)
{
    int r, c, p;
    for (p = 0; p < 4; p++) memset(bar_bm + p * BAR_PLANE, p == 2 ? 0xff : 0, BAR_PLANE);
    memset(bar_tused, 0, sizeof bar_tused);
    for (r = 0; r < 8; r++) for (c = 0; c < 32; c++) bar_cell(r, c);
    bar_drawn_h = 240 - pf_h;
}

/* the game changed bar page cell i (or an attribute cell): redraw those cells of the bitmap */
static void bar_update_cell(UWORD i)
{
    int r, c;
    i &= 0x3ff;
    if (i < 0x100) { bar_cell(i >> 5, i & 31); return; }
    if (i >= 0x3c0 && i < 0x3c8) {                      /* attribute rows 0-1 cover tile rows 0-7 */
        int ax = (i & 7) * 4, ay = ((i - 0x3c0) >> 3) * 4;
        for (r = ay; r < ay + 4 && r < 8; r++) for (c = ax; c < ax + 4; c++) bar_cell(r, c);
    } else if (i >= 0x3c8 && i < 0x3d0) {
        int ax = (i & 7) * 4;
        for (r = 4; r < 8; r++) for (c = ax; c < ax + 4; c++) bar_cell(r, c);
    }
}

static void bar_update_tile(UBYTE t)
{
    int i;
    if (!bar_tused[t]) return;
    for (i = 0; i < 256; i++) if (nes_vram[bar_page | i] == t) bar_cell(i >> 5, i & 31);
}

#ifdef __m68k__
/* The four plane blits are queued: the CPU starts the next one when the blitter is free (bar_pump(), called
   while the sprites are built in fast RAM) instead of waiting for each. */
static int bar_next = 4;
static ULONG bar_off; static UWORD bar_sh, bar_lines;
static void bar_start(int p)
{
    volatile struct Custom *cu = (volatile struct Custom *)0xdff000;
    cu->bltcon0 = (UWORD)(0x09f0 | (bar_sh << 12));
    cu->bltcon1 = 0;
    cu->bltafwm = 0xffff; cu->bltalwm = 0xffff;
    cu->bltapt = bar_bm + p * BAR_PLANE;
    cu->bltdpt = disp_front + p * disp_plane + bar_off;
    cu->bltamod = BAR_ROWB - 2 * BAR_WORDS;
    cu->bltdmod = 64 - 2 * BAR_WORDS;
    cu->bltsize = (UWORD)((bar_lines << 6) | BAR_WORDS);
}
static void bar_pump(void)
{
    volatile struct Custom *cu = (volatile struct Custom *)0xdff000;
    if (bar_next >= 4) return;
    (void)cu->dmaconr;
    if (cu->dmaconr & 0x4000) return;               /* blitter busy */
    bar_start(bar_next++);
}
static void bar_finish(void)
{
    while (bar_next < 4) { blit_wait(); bar_start(bar_next++); }
}
static void blit_bar(LONG wx, int wy, int lines)
{
    ULONG bx = (ULONG)(wx - 32 + disp_margin);
    bar_finish();
    blit_wait();
    bar_off = (ULONG)wy * 64 + ((bx >> 3) & ~1UL);
    bar_sh = (UWORD)(bx & 15);
    bar_lines = (UWORD)lines;
    bar_start(0);
    bar_next = 1;
}
#else                                   /* the same copy in C, for the host harness tools/vhost.c */
static void blit_bar(LONG wx, int wy, int lines)
{
    ULONG bx = (ULONG)(wx - 32 + disp_margin);
    int p, l, x;
    for (p = 0; p < 4; p++)
        for (l = 0; l < lines; l++)
            for (x = 0; x < BAR_WORDS * 16; x++) {
                UBYTE *s = bar_bm + p * BAR_PLANE + l * BAR_ROWB, *d = disp_front + p * disp_plane + (ULONG)(wy + l) * 64;
                ULONG dx = bx + x;
                int bit = (s[x >> 3] >> (7 - (x & 7))) & 1;
                d[dx >> 3] = (UBYTE)((d[dx >> 3] & ~(0x80 >> (dx & 7))) | (bit << (7 - (dx & 7))));
            }
}
static void bar_pump(void) { }
static void bar_finish(void) { }
#endif

static void bar_frame(int sy, int moved)
{
    int r, y0 = sy + pf_h, y1 = sy + 240;
    if (!bar_valid || bar_drawn_h != 240 - pf_h) { render_bar(); bar_valid = 1; moved = 1; }
    if (!moved && bar_x == cam_ux && bar_y == sy && bar_h == pf_h) return;
    blit_bar(cam_ux, y0, y1 - y0);
    bar_x = cam_ux; bar_y = sy; bar_h = pf_h;
    for (r = 0; r < 30; r++) {                          /* the row copies the bar covered must be drawn again */
        int c;
        for (c = 0; c < 2; c++) {                       /* lines below y0 of a row copy are the bar's now */
            int yc = 8 * r + 240 * c;
            if (yc + 8 > y0 && yc < y1 && rowlines[r][c] > (y0 > yc ? y0 - yc : 0)) rowlines[r][c] = (UBYTE)(y0 > yc ? y0 - yc : 0);
        }
    }
}

/* ---------------------------------------------------------------- sprites */
/* Hardware sprites 0-6 (fetch 0). A cluster of NES sprites in one NES palette p goes into sprite 2p or 2p+1
   alone (3 colours: colours 16+4p+1..3 are that palette); a cluster in several palettes into an attached pair
   (0/1, 2/3, 4/5: 15 colours). Each hardware sprite is reused down the screen, a segment per cluster, with
   at least one free line between segments. */
#define NSEG    32                      /* (powers of two: the arrays below are indexed by shifts) */
#define MAXIN   46                      /* sprites a cluster: the struct is then 64 bytes */
typedef struct { WORD x0, x1, y0, y1; UBYTE n, pals; BYTE se, so; UWORD oe, oo; UBYTE ie, io; UBYTE idx[MAXIN]; } Group;
static Group grp[64];
static int ngrp;
static Group *hseg[8][NSEG];            /* the clusters of each hardware sprite, top to bottom */
static int hn[8];
#define SPR_BYTES (16 * (240 + 2 * NSEG + 4))
static UBYTE *spr_mem[2][8];            /* two sets of sprite lists, swapped every frame */
static int spr_set;
int video_sprites_dropped;
ULONG vstat[4];                         /* host statistics: groups, cache hits, NES sprites drawn, merges */
static int spr_split;                   /* under a split, sprite 0 only marks it and the bar shows from BAR_SHOW */

#ifdef __m68k__
int group_scan(Group *grp __asm("a0"), const UBYTE *oam __asm("a1"), ULONG first __asm("d0"), ULONG split __asm("d1"),
               ULONG pfh __asm("d2"), ULONG bar_show __asm("d3"));    /* src/video68k.s: the C below, by hand */
#endif
static void build_groups(void)
{
    int i, j, k;
    WORD gx0 = 0, gx1 = 0, gy0 = 0, gy1 = 0;    /* the newest group, in registers while sprites join it */
    UBYTE gn = 0, gp = 0, *gidx = 0;
    const UBYTE *oam = nes_oam + (spr_split ? 4 : 0);
#ifdef __m68k__
    (void)gx0; (void)gx1; (void)gy0; (void)gy1; (void)gn; (void)gp; (void)gidx; (void)oam;
    ngrp = group_scan(grp, nes_oam, spr_split ? 1 : 0, (ULONG)spr_split, (ULONG)pf_h, BAR_SHOW);
#ifdef GROUP_CHECK                      /* debug: the C version too, and count the frames where they differ */
    {   static Group save[64]; int na = ngrp, m;
        for (m = 0; m < na; m++) save[m] = grp[m];
#endif
#endif
#if !defined(__m68k__) || defined(GROUP_CHECK)
    ngrp = 0;
    for (i = spr_split ? 1 : 0; i < 64; i++, oam += 4) {
        WORD y = oam[0], x = oam[3], sy = (WORD)(y + 1);
        if (y >= 0xef) continue;
        if (spr_split && sy + 8 > pf_h && sy + 4 < BAR_SHOW) continue;   /* in the blank lines between */
        if (gn) {                                       /* a metasprite's sprites come together in OAM */
            WORD nx0 = x < gx0 ? x : gx0, nx1 = x + 8 > gx1 ? x + 8 : gx1;
            if (nx1 - nx0 <= SPR_W && gn < MAXIN && sy <= gy1 + 1 && sy + 8 >= gy0 - 1 && x <= gx1 + 1 && x + 8 >= gx0 - 1) {
                gx0 = nx0; gx1 = nx1;
                if (sy < gy0) gy0 = sy;
                if (sy + 8 > gy1) gy1 = sy + 8;
                gidx[gn++] = (UBYTE)i;
                gp |= (UBYTE)(1 << (oam[2] & 3));
                continue;
            }
            { Group *g = &grp[ngrp - 1]; g->x0 = gx0; g->x1 = gx1; g->y0 = gy0; g->y1 = gy1; g->n = gn; g->pals = gp; }
        }
        for (j = ngrp - 2; j >= 0 && j >= ngrp - 4; j--) {  /* the three groups before it */
            Group *g = &grp[j];
            WORD nx0 = x < g->x0 ? x : g->x0, nx1 = x + 8 > g->x1 ? x + 8 : g->x1;
            if (nx1 - nx0 > SPR_W || g->n >= MAXIN) continue;
            if (sy > g->y1 + 1 || sy + 8 < g->y0 - 1 || x > g->x1 + 1 || x + 8 < g->x0 - 1) continue;
            g->x0 = nx0; g->x1 = nx1;
            if (sy < g->y0) g->y0 = sy;
            if (sy + 8 > g->y1) g->y1 = sy + 8;
            g->idx[g->n++] = (UBYTE)i;
            g->pals |= (UBYTE)(1 << (oam[2] & 3));
            break;
        }
        if (j >= 0 && j >= ngrp - 4) continue;
        gidx = grp[ngrp++].idx;                         /* a new group, the newest */
        gx0 = x; gx1 = x + 8; gy0 = sy; gy1 = sy + 8; gn = 1; gidx[0] = (UBYTE)i;
        gp = (UBYTE)(1 << (oam[2] & 3));
    }
    if (gn) { Group *g = &grp[ngrp - 1]; g->x0 = gx0; g->x1 = gx1; g->y0 = gy0; g->y1 = gy1; g->n = gn; g->pals = gp; }
#endif
#if defined(__m68k__) && defined(GROUP_CHECK)
        if (ngrp != na) vprof[11]++;
        else for (m = 0; m < na; m++)
            if (save[m].x0 != grp[m].x0 || save[m].x1 != grp[m].x1 || save[m].y0 != grp[m].y0 || save[m].y1 != grp[m].y1
                || save[m].n != grp[m].n || save[m].pals != grp[m].pals || memcmp(save[m].idx, grp[m].idx, save[m].n)) { vprof[11]++; break; }
    }
#endif
    for (i = 0; i < ngrp; i++) {                        /* merge groups that touch, while they fit */
        Group *a = &grp[i];
        WORD ax0, ax1, ay0, ay1;
        if (!a->n) continue;
        ax0 = a->x0; ax1 = a->x1; ay0 = a->y0; ay1 = a->y1;
        for (j = i + 1; j < ngrp && j <= i + 6; j++) {
            Group *b = &grp[j];
            WORD nx0, nx1;
            if (b->y0 > ay1 + 1 || b->y1 < ay0 - 1 || b->x0 > ax1 + 1 || b->x1 < ax0 - 1 || !b->n) continue;
            nx0 = ax0 < b->x0 ? ax0 : b->x0; nx1 = ax1 > b->x1 ? ax1 : b->x1;
            if (nx1 - nx0 > SPR_W || a->n + b->n > MAXIN) continue;
            a->x0 = ax0 = nx0; a->x1 = ax1 = nx1;
            if (b->y0 < ay0) a->y0 = ay0 = b->y0;
            if (b->y1 > ay1) a->y1 = ay1 = b->y1;
            for (k = 0; k < b->n; k++) a->idx[a->n++] = b->idx[k];
            a->pals |= b->pals;
            b->n = 0;                                   /* merged away: skipped from here on */
            b->y0 = 0x7fff;
        }
    }
    for (i = j = 0; i < ngrp; i++)                      /* drop the merged-away groups (only the used part moves) */
        if (grp[i].n) {
            if (i != j) { Group *a = &grp[j], *b = &grp[i];
                a->x0 = b->x0; a->x1 = b->x1; a->y0 = b->y0; a->y1 = b->y1; a->n = b->n; a->pals = b->pals;
                for (k = 0; k < b->n; k++) a->idx[k] = b->idx[k]; }
            j++;
        }
    ngrp = j;
}

static WORD busy[8];                    /* first free line of each hardware sprite below its last segment */
#define fits(n, g) (busy[n] <= (g)->y0 && hn[n] < NSEG)
#define place(n, g) (hseg[n][hn[n]++] = (g), busy[n] = (WORD)((g)->y1 + 1))

static void assign(void)
{
    int i, j, k, n;
    Group *order[64];
    for (i = 0; i < 7; i++) { hn[i] = 0; busy[i] = -32767; }
    for (i = 0; i < ngrp; i++) order[i] = &grp[i];
    for (i = 1; i < ngrp; i++) {                      /* by top line */
        Group *t = order[i];
        for (j = i; j > 0 && order[j - 1]->y0 > t->y0; j--) order[j] = order[j - 1];
        order[j] = t;
    }
    for (i = 0; i < ngrp; i++) {
        Group *g = order[i];
        g->se = g->so = -1;
        if (!(g->pals & (g->pals - 1))) {               /* one palette: a sprite of its own colours */
            int p = g->pals == 1 ? 0 : g->pals == 2 ? 1 : g->pals == 4 ? 2 : 3;
            for (n = 2 * p; n <= 2 * p + 1 && n <= 6; n++)
                if (fits(n, g)) { place(n, g); g->se = (BYTE)n; break; }
            if (g->se >= 0) continue;
        }
        for (k = 0; k < 3; k++)
            if (fits(2 * k, g) && fits(2 * k + 1, g)) {
                place(2 * k, g); place(2 * k + 1, g);
                g->se = (BYTE)(2 * k); g->so = (BYTE)(2 * k + 1);
                break;
            }
        if (g->se < 0) video_sprites_dropped++;
    }
}

static const UBYTE *spr_chr(UBYTE tile) { return nes_chr + ((ppu_ctrl & 0x08) ? 0x1000 : 0) + tile * 16; }

static void render_group(Group *g, UBYTE *e, UBYTE *o)
{
    int k;
    for (k = g->n - 1; k >= 0; k--) {           /* highest OAM index first: lower numbers end on top */
        int i = g->idx[k];
        UBYTE tile = nes_oam[i * 4 + 1], at = nes_oam[i * 4 + 2];
        int dx = nes_oam[i * 4 + 3] - g->x0, dy = nes_oam[i * 4] + 1 - g->y0;
        spr_put(e + dy * 16, o ? o + dy * 16 : e, spr_chr(tile),
                (ULONG)(dx | ((at & 0x40) << 2) | ((at & 0x80) << 2) | ((at & 3) << 10) | (o ? 0x1000 : 0)));
    }
}

static void clear_lines(UBYTE *p, int n)
{
    ULONG *l = (ULONG *)p;
    for (n >>= 4; n; n--) { l[0] = 0; l[1] = 0; l[2] = 0; l[3] = 0; l += 4; }
}

/* Rendered clusters are kept in a pool (fast RAM if there is any), found again by a signature of their content:
   the tiles' pixels (a hash of each CHR RAM tile, so the boat's rotation frames, uploaded again and again into
   the same tiles, are recognised), attributes and layout. A hit is copied straight into the sprite list. */
#ifndef NOSKIP
#define NOSKIP 0
#endif
#define PC_LINES 64                     /* taller clusters are drawn every time */
#define PC_BYTES (16 * PC_LINES)
typedef struct { ULONG sig, last; UWORD n; UBYTE *e, *o; } PEnt;
static PEnt *pc;
static int pcn;                         /* entries: a power of two */
static ULONG vframe;
static ULONG slot_key[2][8][NSEG];      /* what each segment slot of each sprite list holds (0: unknown) */
static UWORD slot_off[2][8][NSEG];
static ULONG thash[512];                /* content hash of every CHR tile */
static UBYTE scr_e[16 * 256], scr_o[16 * 256];    /* clusters that are not kept are put together here */

static ULONG tile_hash(const UBYTE *t)
{
    ULONG h = 0x811c9dc5UL; int i;
    for (i = 0; i < 16; i++) h = ((h << 7) | (h >> 25)) ^ t[i];
    return h;
}

static ULONG group_sig(Group *g, int with_o)
{
    ULONG h = (ULONG)g->n * 0x9e3779b1UL ^ (ULONG)(g->y1 - g->y0) ^ ((ULONG)with_o << 31);
    const ULONG *th = thash + ((ppu_ctrl & 0x08) ? 256 : 0);
    int k;
    for (k = 0; k < g->n; k++) {
        const UBYTE *o = nes_oam + g->idx[k] * 4;
        ULONG v = ((ULONG)o[2] << 16) | ((ULONG)(UBYTE)(o[3] - g->x0) << 8) | (UBYTE)(o[0] + 1 - g->y0);
        h = ((h << 5) | (h >> 27)) + (v ^ th[o[1]]);   /* no multiply: there is no fast one on this CPU */
    }
    return h;
}

/* the pool entry for sig: found (*hit = 1), or a free / least recently used one to fill, or 0 (none free) */
static PEnt *pool_find(ULONG sig, UWORD n, int *hit)
{
    int k, i = (int)((sig ^ (sig >> 13)) & (ULONG)(pcn - 1));
    PEnt *best = 0;
    *hit = 0;
    if (n > PC_BYTES) return 0;
    for (k = 0; k < 8; k++, i = (i + 1) & (pcn - 1)) {
        PEnt *p = &pc[i];
        if (p->sig == sig && p->n == n && p->last) { *hit = 1; p->last = vframe; return p; }
        if (p->last == vframe) continue;                /* in use this frame */
        if (!best || p->last < best->last) best = p;
    }
    if (best) { best->sig = sig; best->n = n; best->last = vframe; }
    return best;
}

static void copy_lines(UBYTE *d, const UBYTE *s, int n)
{
    ULONG *l = (ULONG *)d; const ULONG *m = (const ULONG *)s;
    for (n >>= 4; n; n--) { l[0] = m[0]; l[1] = m[1]; l[2] = m[2]; l[3] = m[3]; l += 4; m += 4; }
}

static void put_ctl(UWORD n, UBYTE *d, Group *g, int att)
{
    WORD x = (WORD)(g->x0 + VIEW_X), y = g->y0, h = (WORD)(g->y1 - g->y0);
    UWORD hs = (UWORD)(x + 0x80), vs = (UWORD)(y + 0x34), ve = (UWORD)(vs + h);
    if (d == spr_mem[spr_set][n]) { disp_sprite(n, (UWORD *)d, x, y, (UWORD)h, (UWORD)att); return; }   /* the first: the kit */
    PUTW(d, ((vs & 0xff) << 8) | ((hs >> 1) & 0xff));
    PUTW(d + 8, ((ve & 0xff) << 8) | (att ? 0x80 : 0) | (((vs >> 8) & 1) << 2) | (((ve >> 8) & 1) << 1) | (hs & 1));
}

static void build_sprites(void)
{
    int k, s, n;
    UWORD off;
    spr_set ^= 1;
    VP(3, { build_groups(); assign(); });
    for (k = 0; k < 7; k++) {                           /* where each segment goes in its sprite's list */
        off = 0;
        for (s = 0; s < hn[k]; s++) {
            Group *g = hseg[k][s];
            if (g->se == k) { g->oe = off; g->ie = (UBYTE)s; } else { g->oo = off; g->io = (UBYTE)s; }
            off = (UWORD)(off + 16 + (g->y1 - g->y0) * 16);
        }
        if (!hn[k]) disp_sprite_off((UWORD)k);
        else memset(spr_mem[spr_set][k] + off, 0, 16);  /* end of the list */
        for (s = hn[k]; s < NSEG; s++) slot_key[spr_set][k][s] = 0;   /* slots beyond the list: overwritten or stale */
    }
    vframe++;
    for (k = 0; k < ngrp; k++) {
        Group *g = &grp[k];
        UBYTE *e, *o, *ae, *ao;
        ULONG sig;
        PEnt *pe;
        int hit;
        bar_pump();
        if (g->se < 0) continue;
        n = (g->y1 - g->y0) * 16;
        e = spr_mem[spr_set][g->se] + g->oe;
        o = g->so >= 0 ? spr_mem[spr_set][g->so] + g->oo : 0;
        VP(8, {
        put_ctl((UWORD)g->se, e, g, 0);
        if (o) put_ctl((UWORD)g->so, o, g, 1);
        sig = group_sig(g, o != 0);
        pe = pool_find(sig, (UWORD)n, &hit); });
        vstat[0]++;
        if (pe) { ae = pe->e; ao = pe->o; } else { ae = scr_e; ao = scr_o; }
        if (hit) vstat[1]++;
        else VP(9, {
            clear_lines(ae, n);
            if (o) clear_lines(ao, n);
            render_group(g, ae, o ? ao : 0); vstat[2] += g->n; });
        {   /* a list slot that already holds this cluster's pixels (the same set, two frames ago) is not copied */
            ULONG key = sig ^ (ULONG)n ^ (o ? 0x80000000UL : 0);
            ULONG *ke = &slot_key[spr_set][g->se][g->ie];
            VP(10, {
            if (NOSKIP || *ke != key || slot_off[spr_set][g->se][g->ie] != g->oe) { copy_lines(e + 16, ae, n); vstat[3]++; }
            *ke = key; slot_off[spr_set][g->se][g->ie] = g->oe;
            if (o) {
                ULONG *ko = &slot_key[spr_set][g->so][g->io];
                if (NOSKIP || *ko != key || slot_off[spr_set][g->so][g->io] != g->oo) copy_lines(o + 16, ao, n);
                *ko = key; slot_off[spr_set][g->so][g->io] = g->oo;
            } });
        }
    }
}

/* ---------------------------------------------------------------- frame */
static ULONG pal_rgb[32];
static int pal_loaded;
static UBYTE spr_valid, was_on, last_pal0;

static void set_palette(int on, UBYTE pal0)
{
    int i;
    for (i = 0; i < 32; i++) {
        UBYTE c = (i & 3) ? nes_pal[i] : nes_pal[0];   /* 4, 8, 12: the backdrop below the split (the bar's colour 0) */
        ULONG rgb;
        if (i == 0) c = pal0;
        rgb = on ? nes_rgb[c & 0x3f] : 0;
        if (rgb != pal_rgb[i] || !pal_loaded) { pal_rgb[i] = rgb; disp_palette(&pal_rgb[i], (UWORD)i, 1); }   /* only what changed */
    }
    pal_loaded = 1;
}

int video_load(void)
{
    int i, j;
    for (i = 0; i < 256; i++) { UBYTE r = 0; for (j = 0; j < 8; j++) if (i & (1 << j)) r |= (UBYTE)(0x80 >> j); video_flip[i] = r; }
    for (i = 0; i < 2; i++)
        for (j = 0; j < 7; j++)
            if (!(spr_mem[i][j] = sys_alloc(SPR_BYTES, MEMF_CHIP | MEMF_CLEAR))) return 0;
    if (!(bar_bm = sys_alloc(4 * BAR_PLANE, MEMF_CHIP | MEMF_CLEAR))) return 0;
    /* the cluster pool: 256 entries (512 KB) with fast RAM, 16 in chip RAM without */
    pcn = AvailMem(MEMF_FAST) > 2 * 1024 * 1024 ? 256 : 16;
    if (!(pc = sys_alloc(pcn * sizeof(PEnt), MEMF_ANY | MEMF_CLEAR))) return 0;
    for (i = 0; i < pcn; i++) {
        if (!(pc[i].e = sys_alloc(2 * PC_BYTES, MEMF_ANY))) return 0;
        pc[i].o = pc[i].e + PC_BYTES;
    }
    video_plane = 0;
    invalidate();
    return 1;
}

void video_start(void)
{
    int t;
    for (t = 0; t < 512; t++) thash[t] = tile_hash(nes_chr + t * 16);
    video_plane = disp_plane;
    cam_ux = LEVEL_W / 2;
    sx_prev = -1;
    invalidate();
}

void video_frame(void)
{
    int split = split_on, i, budget, bar_dirty = 0, spr_chr_dirty = 0;
    UBYTE ctl = split ? split_ctrl : ppu_ctrl, mask = split ? split_mask : ppu_mask;
    UWORD t = split ? split_t : ppu_t, page = (UWORD)((((split ? split_ax : ax_reg) & 0x10) ? 0x400 : 0));
    UBYTE fx = split ? split_fx : ppu_fx, pal0 = split ? split_pal0 : nes_pal[0];
    int on = (mask & 0x18) != 0;
    WORD sx = (WORD)(((t & 0x1f) << 3) | fx);
    int sy = ((((t >> 5) & 0x1f) * 8) + ((t >> 12) & 7)) % 240;
    LONG vlo, vhi;

    bar_finish();                                       /* the buffer is drawn into below: no blit may still run */
    blit_wait();
    pf_h = split ? (split_line + 2 > 240 ? 240 : split_line + 2) : 240;
    spr_split = split;
    mask_left = mask;
    cur_sy = sy;
    /* camera: follow the NES scroll by its change since the last frame (one screen: it wraps at 256) */
    if (sx_prev >= 0) {
        WORD d = (WORD)((sx - sx_prev) & 255);
        if (d >= 128) d -= 256;
        cam_ux += d;
    } else cam_ux = LEVEL_W / 2 + sx;
    sx_prev = sx;
    if (cam_ux < 256 || cam_ux > LEVEL_W - 512) { cam_ux = LEVEL_W / 2 + (sx & 255); invalidate(); }

    bgsel = (ctl & 0x10) != 0;
    pf_page = page;
    if (nes_redraw_all || bgsel != drawn_bgsel || page != drawn_page) {
        drawn_bgsel = bgsel; drawn_page = page; nes_redraw_all = 0;
        invalidate(); bar_valid = 0;
    }
    if (split) {
        UWORD bp = (UWORD)((ax_reg & 0x10) ? 0x400 : 0);
        if (bp != bar_page) { bar_page = bp; bar_valid = 0; }
    }

    /* CHR RAM tiles the game rewrote */
    VP(4, for (i = 0; i < chr_ndirty; i++) {
        UWORD tl = chr_dirty[i];
        chr_dirty_flag[tl] = 0;
        if ((tl >> 8) == bgsel) {
            if (bar_valid && bar_tused[tl & 255]) { bar_update_tile((UBYTE)tl); bar_dirty = 1; }
            if (chr_ndirty > 12) { if (col_lo <= col_hi) invalidate(); }   /* a new tile set: draw everything again */
            else if (col_lo <= col_hi) redraw_tile((UBYTE)tl);
        }
        thash[tl] = tile_hash(nes_chr + tl * 16);
        if ((tl >> 8) == ((ppu_ctrl >> 3) & 1)) spr_chr_dirty = 1;
    });
    chr_ndirty = 0;

    VP(1, sync_rows(sy));

    /* nametable cells the game wrote (a whole new screen: draw it again column by column) */
    if (nes_ndirty > 200) { invalidate(); bar_valid = 0; }
    for (i = 0; i < nes_ndirty; i++) {
        UWORD c = nes_dirty[i];
        nes_dirty_flag[c] = 0;
        if (nes_ndirty > 200) continue;
        if ((c & 0x400) == pf_page && col_lo <= col_hi) VP(0, redraw_cell(c));
        if (split && bar_valid && (c & 0x400) == bar_page && ((c & 0x3ff) < 0x100 || (c & 0x3ff) >= 0x3c0)) { bar_update_cell(c); bar_dirty = 1; }
    }
    nes_ndirty = 0;

    /* while the visible part is incomplete (a new screen), show black and draw quickly */
    want_cols(&vlo, &vhi);
    budget = (on && col_lo <= vlo && col_hi >= vhi) ? 24 : 120;
    VP(1, stream_cols(budget));
    if (!(col_lo <= vlo && col_hi >= vhi)) on = 0;
    VP(6, if (col_lo <= col_hi) fix_edges());

    if (nes_pal_dirty || on != was_on || pal0 != last_pal0) { set_palette(on, pal0); nes_pal_dirty = 0; was_on = (UBYTE)on; last_pal0 = pal0; }
    disp_scroll(cam_ux - VIEW_X, sy);
    if ((mask & 0x10) && on) {
        static ULONG last_oam[64];
        static UBYTE last_state[4];
        const ULONG *oam4 = (const ULONG *)nes_oam;
        int same = 1;
        UBYTE st[4];
        st[0] = ppu_ctrl; st[1] = (UBYTE)split; st[2] = (UBYTE)pf_h; st[3] = 0;
        for (i = 0; i < 64; i++) if (last_oam[i] != oam4[i]) { same = 0; break; }
        if (!spr_valid || spr_chr_dirty || !same || memcmp(last_state, st, 4)) {
            VP(2, build_sprites());
            for (i = 0; i < 64; i++) last_oam[i] = oam4[i];
            memcpy(last_state, st, 4); spr_valid = 1;
        }
    }
    else { for (i = 0; i < 7; i++) disp_sprite_off((UWORD)i); spr_valid = 0; }
    /* the status bar last: its blits run on while the CPU goes on with the next ticks in fast RAM */
    if (split && on) VP(5, bar_frame(sy, bar_dirty));
    else bar_x = -1;
#ifndef __m68k__
    {   int r, c; extern int vhost_frame;                /* host check: every shown row copy has its lines */
        for (r = 0; r < 30; r++) for (c = 0; c < 2; c++) {
            int yc = 8 * r + 240 * c, n = need_lines(r, c, sy);

            if (on && col_lo <= col_hi && rowlines[r][c] < n && getenv("VCHECK"))
                fprintf(stderr, "frame %d row %d copy %d lines %d need %d sy %d\n", vhost_frame, r, c, rowlines[r][c], n, sy);
        } }
#endif
}
