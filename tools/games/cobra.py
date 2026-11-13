"""cobra.py - what the translator needs to know about this game that it cannot find out by itself.
Loaded by tools/recomp.py (--game cobra). See tools/games/llb.py for a filled-in example and PLAYBOOK-NES.md
sections 3-5 for how to find each entry.
"""
NORETURN = set()        # subroutines that pop their own return address (jump-table dispatchers)
HLE = {}                # (bank, address): hand-written 68k text, exact, proven by the checksum
HLE_FALLBACK = set()    # entries of HLE whose text may branch to .orig
