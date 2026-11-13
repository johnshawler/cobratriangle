"""cobra.py - what the translator needs to know about Cobra Triangle (Rare 1989, PAL, AxROM) that it cannot find
out by itself. Loaded by tools/recomp.py (--game cobra). See tools/games/llb.py for an MMC1 example and
PLAYBOOK-NES.md sections 3-5 for how to find each entry.
"""
NORETURN = set()        # subroutines that pop their own return address (jump-table dispatchers)

# $FFB0 (the same in all four banks): DEX/BNE, DEY/BNE delay loop, X*Y iterations, used for raster timing.
# The port has no raster timing: the result is X = 0, Y = 0, Z set, N clear, C and V unchanged.
_DELAY = '''
    moveq #0,d1
    moveq #0,d2
'''
HLE = {(b, 0xffb0): _DELAY for b in range(4)}  # (bank, address): hand-written 68k text, exact, proven by the checksum
HLE_FALLBACK = set()    # entries of HLE whose text may branch to .orig

# $FFA1 / $FFA9 in bank 2 (reached by JSR $FF9C / $FFA4 from bank 0, which switch to bank 2): the boat's CHR RAM
# upload. Y passes of 16 bytes: byte k of a pass is ROM[table_k + X] (16 tables $100 apart, from $DE00 or
# $EE00), written to $2007; X+1 between passes; then STY $8000 (Y = 0: back to bank 0) and RTS.
# By hand: straight into CHR RAM with the same dirty-tile marks as the $2007 path. Ends with A = the last byte,
# X advanced, Y = 0, N clear, Z set, C and V unchanged. Falls back to the translation unless the writes are
# +1 apart and stay below $2000 (CHR RAM).
def _upload(tab):
    return '''
    move.w _ppu_v,d3
    and.w #$3fff,d3
    btst #2,_ppu_ctrl
    bne .orig
    move.l d2,d7
    and.w #$ff,d7
    beq .orig
    lsl.w #4,d7
    move.w d3,d4
    add.w d7,d4
    cmp.w #$2000,d4
    bhi .orig
    move.w ccr,-(sp)
    move.l a2,-(sp)
    lea _nes_chr_ram,a0
    adda.w d3,a0
.it lea $%x(a3),a1
    adda.l d1,a1
    moveq #15,d3
.by move.b (a1),d0
    lea $100(a1),a1
    cmp.b (a0),d0
    beq.s .same
    move.b d0,(a0)
    move.l a0,d6
    sub.l #_nes_chr_ram,d6
    lsr.w #4,d6
    lea _chr_dirty_flag,a2
    tst.b (a2,d6.w)
    bne.s .same
    move.b #1,(a2,d6.w)
    move.w _chr_ndirty,d4
    add.w d4,d4
    lea _chr_dirty,a2
    move.w d6,(a2,d4.w)
    addq.w #1,_chr_ndirty
.same addq.l #1,a0
    dbf d3,.by
    subq.b #1,d2
    beq.s .end
    addq.b #1,d1
    bra.s .it
.end add.w d7,_ppu_v
    move.l (sp)+,a2
    move.w (sp)+,ccr
    moveq #0,d2
    movea.l #$8000,a0
    bsr _io_wr_d2
    jmp D_ffaf
''' % tab
HLE[(2, 0xffa1)] = _upload(0xde00 - 0x8000)
HLE[(2, 0xffa9)] = _upload(0xee00 - 0x8000)
HLE_FALLBACK |= {(2, 0xffa1), (2, 0xffa9)}
