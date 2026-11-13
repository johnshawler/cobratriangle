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
