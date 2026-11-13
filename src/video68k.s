; video68k.s - the two inner loops of src/video.c, in assembly (they run every frame and stay in the cache).
        section code,code
SPRW    equ     32                      ; = SPR_W in src/video.c
SPRLB   equ     SPRW/4                  ; bytes a sprite line (two planes)
SPRHB   equ     SPRW/8
        xdef    _tile8,_spr_put
        xref    _video_plane,_video_flip

; void tile8(UBYTE *d __asm("a0"), const UBYTE *chr __asm("a1"), ULONG attr __asm("d0"))
; One 8x8 NES tile into 4 bitplanes, 64 bytes a line: planes 0-1 from the CHR tile (8 bytes plane 0,
; 8 bytes plane 1), planes 2-3 the attribute bytes in attr bits 0-7 and 8-15, only under the tile's
; non-zero pixels: a colour-0 pixel is colour 0 whatever its palette (the NES shows the backdrop there).
_tile8: movem.l d2-d5/a2-a4,-(sp)
        move.l  _video_plane,d2
        lea     (a0,d2.l),a2
        lea     (a2,d2.l),a3
        lea     (a3,d2.l),a4
        move.w  d0,d1
        lsr.w   #8,d1                   ; plane 3 byte
        moveq   #7,d3
.l      move.b  8(a1),d4
        move.b  (a1)+,d5
        move.b  d5,(a0)
        move.b  d4,(a2)
        or.b    d5,d4
        move.b  d4,d5
        and.b   d0,d4
        and.b   d1,d5
        move.b  d4,(a3)
        move.b  d5,(a4)
        lea     64(a0),a0
        lea     64(a2),a2
        lea     64(a3),a3
        lea     64(a4),a4
        dbf     d3,.l
        movem.l (sp)+,d2-d5/a2-a4
        rts

; void spr_put(UBYTE *e __asm("a0"), UBYTE *o __asm("a1"), const UBYTE *chr __asm("a2"), ULONG ctl __asm("d0"))
; One 8x8 NES sprite into the lines of a 64-pixel attached sprite pair, over what is there.
; e, o: the sprite's first line in the even and odd sprite's data (16 bytes a line: 8 plane A, 8 plane B).
; ctl: bits 0-5 x in the 64-pixel window, 8 h-flip, 9 v-flip, 10-11 NES sprite palette, 12 o is valid.
_spr_put:
        movem.l d2-d7/a3,-(sp)
        move.l  d0,d7
        moveq   #0,d1
        move.b  d7,d1
        move.w  d1,d2
        lsr.w   #3,d2
        adda.w  d2,a0
        adda.w  d2,a1
        and.w   #7,d1
        moveq   #8,d6
        sub.w   d1,d6                   ; shift: 8 - x mod 8
        lea     _video_flip,a3
        moveq   #7,d5
        moveq   #1,d4
        btst    #9,d7
        beq.s   .row
        addq.l  #7,a2
        moveq   #-1,d4
.row    moveq   #0,d1
        move.b  (a2),d1
        moveq   #0,d2
        move.b  8(a2),d2
        btst    #8,d7
        beq.s   .nh
        move.b  (a3,d1.w),d1
        move.b  (a3,d2.w),d2
.nh     move.w  d1,d3
        or.w    d2,d3
        beq.s   .next
        lsl.w   d6,d1
        lsl.w   d6,d2
        lsl.w   d6,d3                   ; opaque pixels
        move.w  d3,d0
        not.w   d0                      ; keep mask
        and.w   d0,(a0)
        or.w    d1,(a0)
        and.w   d0,SPRHB(a0)
        or.w    d2,SPRHB(a0)
        btst    #12,d7
        beq.s   .next
        and.w   d0,(a1)
        btst    #10,d7
        beq.s   .p2
        or.w    d3,(a1)
.p2     and.w   d0,SPRHB(a1)
        btst    #11,d7
        beq.s   .next
        or.w    d3,SPRHB(a1)
.next   lea     SPRLB(a0),a0
        lea     SPRLB(a1),a1
        adda.w  d4,a2
        dbf     d5,.row
        movem.l (sp)+,d2-d7/a3
        rts

; int group_scan(Group *grp a0, const UBYTE *oam a1, ULONG first d0, ULONG split d1, ULONG pf_h d2, ULONG bar_show d3)
; The first pass of build_groups() in src/video.c: NES sprites (OAM from entry 'first') into clusters of at most
; 64 pixels; a sprite joins the newest cluster if it touches it, else one of the three before, else starts one.
; Group: x0 0, x1 2, y0 4, y1 6, n 8, pals 9, idx 18.. (64 bytes). Returns the number of clusters.
        xdef    _group_scan
GSZ     equ     64
MAXIN   equ     46
_group_scan:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  a0,a2                   ; grp
        move.w  d2,a5                   ; pf_h
        move.w  d3,a6                   ; bar_show
        move.l  d1,-(sp)                ; split flag at (sp)
        moveq   #0,d1
        move.b  d0,d1                   ; i
        lea     (a1,d1.w*4),a0          ; oam entry
        moveq   #0,d6                   ; gn = 0: no current group
        suba.l  a3,a3                   ; number of groups * 64 (an offset)
.next   cmp.w   #64,d1
        bcc     .done
        moveq   #0,d0
        move.b  (a0),d0                 ; y
        cmp.b   #$ef,d0
        bcc     .skip
        addq.w  #1,d0                   ; sy
        tst.l   (sp)
        beq.s   .ok
        move.w  d0,a4
        addq.w  #8,a4
        cmpa.w  a5,a4                   ; sy + 8 > pf_h
        ble.s   .ok
        move.w  d0,a4
        addq.w  #4,a4
        cmpa.w  a6,a4                   ; sy + 4 < bar_show: in the blank lines
        blt     .skip
.ok     moveq   #0,d7
        move.b  3(a0),d7                ; x (word)
        tst.w   d6
        beq     .old
        ; the newest group, in d2 x0, d3 x1, d4 y0, d5 y1, d6 n, a1 = its struct
        move.w  d7,a4                   ; nx0 = min(x, x0)
        cmp.w   d2,d7
        blt.s   .m0
        move.w  d2,a4
.m0     move.w  d7,d0
        addq.w  #8,d0                   ; x + 8 (sy is needed again: keep it on the stack)
        move.w  d0,-(sp)
        cmp.w   d3,d0                   ; nx1 = max(x + 8, x1)
        bgt.s   .m1
        move.w  d3,d0
.m1     sub.w   a4,d0                   ; nx1 - nx0
        cmp.w   #SPRW,d0
        bgt     .nofit
        cmp.w   #MAXIN,d6
        bcc     .nofit
        moveq   #0,d0
        move.b  (a0),d0
        addq.w  #1,d0                   ; sy
        move.w  d5,-(sp)
        addq.w  #1,(sp)
        cmp.w   (sp)+,d0                ; sy > y1 + 1
        bgt     .nofit
        move.w  d0,-(sp)
        addq.w  #8,(sp)
        move.w  d4,-(sp)
        subq.w  #1,(sp)
        move.w  (sp)+,a4                ; y0 - 1
        cmpa.w  (sp)+,a4                ; sy + 8 < y0 - 1
        bgt     .nofit
        move.w  d3,a4
        addq.w  #1,a4
        cmpa.w  d7,a4                   ; x > x1 + 1
        blt     .nofit
        move.w  d2,a4
        subq.w  #1,a4
        cmpa.w  (sp),a4                 ; x + 8 < x0 - 1
        bgt     .nofit
        ; join the newest group
        cmp.w   d2,d7
        bge.s   .j0
        move.w  d7,d2
.j0     move.w  (sp)+,a4
        cmpa.w  d3,a4
        ble.s   .j1
        move.w  a4,d3
.j1     cmp.w   d4,d0
        bge.s   .j2
        move.w  d0,d4
.j2     addq.w  #8,d0
        cmp.w   d5,d0
        ble.s   .j3
        move.w  d0,d5
.j3     move.b  d1,18(a1,d6.w)          ; idx[n++] = i
        addq.w  #1,d6
        move.b  2(a0),d0
        and.w   #3,d0
        bset    d0,9(a1)                ; pals
        bra     .skip
.nofit  addq.l  #2,sp
        move.w  d2,(a1)                 ; flush the newest group into its struct
        move.w  d3,2(a1)
        move.w  d4,4(a1)
        move.w  d5,6(a1)
        move.b  d6,8(a1)
.old    ; the three groups before the newest (in memory)
        move.l  a3,d0
        sub.l   #2*GSZ,d0               ; offset of group ngrp - 2
        moveq   #3,d3                   ; at most three (d3 is free: the newest is in memory now)
.oloop  tst.l   d0
        bmi     .new
        lea     (a2,d0.l),a4
        moveq   #0,d2
        move.b  (a0),d2
        addq.w  #1,d2                   ; sy
        ; nx0 = min(x, g->x0), nx1 = max(x + 8, g->x1)
        move.w  (a4),d4
        cmp.w   d7,d4
        ble.s   .o0
        move.w  d7,d4
.o0     move.w  d7,d5
        addq.w  #8,d5
        cmp.w   2(a4),d5
        bge.s   .o1
        move.w  2(a4),d5
.o1     move.w  d5,d6
        sub.w   d4,d6
        cmp.w   #SPRW,d6
        bgt.s   .onext
        cmp.b   #MAXIN,8(a4)
        bcc.s   .onext
        move.w  6(a4),d6
        addq.w  #1,d6
        cmp.w   d6,d2                   ; sy > y1 + 1
        bgt.s   .onext
        move.w  4(a4),d6
        subq.w  #1,d6
        move.w  d2,-(sp)
        addq.w  #8,(sp)
        cmp.w   (sp)+,d6                ; sy + 8 < y0 - 1
        bgt.s   .onext
        move.w  2(a4),d6
        addq.w  #1,d6
        cmp.w   d6,d7                   ; x > x1 + 1
        bgt.s   .onext
        move.w  (a4),d6
        subq.w  #1,d6
        move.w  d7,-(sp)
        addq.w  #8,(sp)
        cmp.w   (sp)+,d6                ; x + 8 < x0 - 1
        bgt.s   .onext
        move.w  d4,(a4)                 ; join it
        move.w  d5,2(a4)
        cmp.w   4(a4),d2
        bge.s   .o2
        move.w  d2,4(a4)
.o2     addq.w  #8,d2
        cmp.w   6(a4),d2
        ble.s   .o3
        move.w  d2,6(a4)
.o3     moveq   #0,d6
        move.b  8(a4),d6
        move.b  d1,18(a4,d6.w)
        addq.b  #1,8(a4)
        move.b  2(a0),d6
        and.w   #3,d6
        bset    d6,9(a4)
        bra.s   .reload
.onext  sub.l   #GSZ,d0
        subq.w  #1,d3
        bne     .oloop
.new    lea     (a2,a3.l),a1            ; a new group: the newest
        adda.w  #GSZ,a3
        move.w  d7,d2                   ; x0
        move.w  d7,d3
        addq.w  #8,d3                   ; x1
        moveq   #0,d4
        move.b  (a0),d4
        addq.w  #1,d4                   ; y0
        move.w  d4,d5
        addq.w  #8,d5                   ; y1
        moveq   #1,d6
        move.b  d1,18(a1)
        move.b  2(a0),d0
        and.w   #3,d0
        clr.b   9(a1)
        bset    d0,9(a1)
        bra.s   .skip
.reload tst.l   a3                      ; joined an older group: the newest goes back into registers
        beq.s   .skip0
        lea     -GSZ(a2,a3.l),a1
        movem.w (a1),d2-d5              ; (sign-extended words: fine)
        moveq   #0,d6
        move.b  8(a1),d6
        bra.s   .skip
.skip0  moveq   #0,d6
.skip   addq.w  #1,d1
        addq.l  #4,a0
        bra     .next
.done   tst.w   d6
        beq.s   .d1
        move.w  d2,(a1)
        move.w  d3,2(a1)
        move.w  d4,4(a1)
        move.w  d5,6(a1)
        move.b  d6,8(a1)
.d1     addq.l  #4,sp
        move.l  a3,d0
        lsr.l   #6,d0                   ; groups
        movem.l (sp)+,d2-d7/a2-a6
        rts
