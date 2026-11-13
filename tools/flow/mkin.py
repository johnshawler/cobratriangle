import random, sys
# mkin.py mode frames seed - input scripts for tools/nesref -i (Cobra Triangle)
#   play: START through the title, then random steering, fire (A), turbo (B), sometimes START (pause)
#   idle: no input (title, high scores, title again)
mode, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); r = random.Random(seed)
out = []
if mode == 'play':
    out += ['600 605 START', '700 705 START', '800 805 START', '900 905 START']
    f = 1000
    while f < n:
        d = r.randint(3, 60)
        b = r.choice(['A', 'B', 'A', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'A LEFT', 'A RIGHT', 'A UP', 'B UP', 'UP LEFT',
                      'UP RIGHT', 'DOWN LEFT', 'DOWN RIGHT', 'A UP RIGHT', 'START' if r.random() < 0.05 else 'A', '', ''])
        if b: out.append('%d %d %s' % (f, f + d, b))
        f += d + r.randint(1, 20)
        if r.random() < 0.01: out.append('%d %d START' % (f, f + 3)); f += 10
print('\n'.join(out))
