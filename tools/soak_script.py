#!/usr/bin/env python3
"""Writes a LOONY_SCRIPT that keeps the game playing: every minute it starts a
game (Esc, Esc, Return, Return), and in between pulls the plunger, works the
flippers and nudges. A screenshot every 5 minutes goes to <shot prefix>NN.png,
and the script asks the game to quit a second before the end.

    tools/soak_script.py <ticks> <shot prefix> [monster-fair] > soak.txt

For MONSTER FAIR the first game starts at tick 3000, once its table plays by
itself (before that, Esc opens the menu at once and the second Esc closes it),
and the nudges take turns with its side nudges, X and period. In a game the
same keys pause it and resume it; its tryout game ends by itself after 100
seconds, so a new one starts every two or three minutes.
"""
import sys

ticks, prefix = int(sys.argv[1]), sys.argv[2]
mf = sys.argv[3:] == ['monster-fair']
if sys.argv[3:] and not mf:
    sys.exit(f'soak_script.py: unknown game {sys.argv[3]}')
nudges = ('space', 'x', 'period') if mf else ('space',)
lines, t, n = [], 3000 if mf else 1700, 0
while t < ticks - 600:
    for key in ('esc', 'esc', 'return', 'return'):
        lines += [(t, f'down {key}'), (t + 4, f'up {key}')]
        t += 90
    end = t + 3600
    while t < end:
        lines += [(t, 'down return'), (t + 80, 'up return')]
        t += 120
        for i in range(12):
            key = 'z' if i % 2 == 0 else 'slash'
            lines += [(t, f'down {key}'), (t + 8, f'up {key}')]
            t += 25
        if (t // 1000) % 7 == 0:
            key = nudges[n % len(nudges)]
            n += 1
            lines += [(t, f'down {key}'), (t + 5, f'up {key}')]
            t += 20
for minute in range(1, (ticks - 61) // 3600 + 1, 5):  # all before the quit
    lines.append((minute * 3600, f'screenshot {prefix}{minute:02d}.png'))
lines.append((ticks - 60, 'quit'))
lines.sort(key=lambda a: a[0])
sys.stdout.write(''.join(f'{t} {a}\n' for t, a in lines))
