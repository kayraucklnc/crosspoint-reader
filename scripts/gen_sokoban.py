#!/usr/bin/env python3
"""Build the Sokoban game's SD-card level pack from David W. Skinner's Microban.

Source: Microban (155 puzzles, revised April 2000) by David W. Skinner. His
sets are freely redistributed by Sokoban programs with credit to the author;
this pack is built for personal use. The text copy used here is the Let's
Logic export mirrored at SOURCE_URL. The converted pack is NOT committed; run
this script to generate it (the par search takes a few minutes):

    python3 scripts/gen_sokoban.py OUT_DIR [OUT_DIR ...] [--source FILE] [--solve-seconds N]
                                   [--solutions FILE | --reuse FILE]

e.g. OUT_DIR = fs_/.crosspoint/games/sokoban (simulator) or
<sd-extra>/.crosspoint/games/sokoban.

Output: levels.bin + NOTICE.txt. levels.bin layout (little endian):
  "SKB1", u16 levelCount, u8 nameLength, name (UTF-8),
  u32 offset[levelCount] (from file start), then per level:
  u8 width, u8 height, u16 parMoves, u16 parPushes (0 = unknown),
  ceil(width * height / 2) bytes of 4-bit cells, row-major, high nibble first.
Cell codes: 0 outside, 1 wall, 2 floor, 3 goal, 4 box, 5 box on goal,
6 player, 7 player on goal.

Par is the move-optimal solution length found by an A* search (pushes break
ties); levels the search cannot finish within the time budget keep par 0.
"""

import argparse
import heapq
import struct
import sys
import time
import urllib.request
from pathlib import Path

SOURCE_URL = ("https://raw.githubusercontent.com/danzinho007/Sokoban/master/"
              "Formato%20txt/Microban%2001%20(6).txt")
PACK_NAME = "Microban"
NOTICE = """Sokoban levels: Microban (155 puzzles, revised April 2000)
by David W. Skinner. Redistributed with credit to the author, for personal use.
Par values were computed by scripts/gen_sokoban.py (move-optimal A* search).
"""
MAX_SIDE = 40
MAX_CELLS = 1024
DIRS = ((0, -1, "u"), (0, 1, "d"), (-1, 0, "l"), (1, 0, "r"))


def parse_levels(text: str) -> list:
    levels, cur = [], []
    for raw in text.splitlines():
        line = raw.rstrip()
        if line and set(line) <= set("#-_ .$*@+"):
            cur.append(line.replace("-", " ").replace("_", " "))
        elif cur:
            levels.append(cur)
            cur = []
    if cur:
        levels.append(cur)
    return levels


class Level:
    def __init__(self, rows: list):
        self.h = len(rows)
        self.w = max(len(r) for r in rows)
        if self.w > MAX_SIDE or self.h > MAX_SIDE or self.w * self.h > MAX_CELLS:
            raise ValueError(f"level too large: {self.w}x{self.h}")
        grid = [r.ljust(self.w) for r in rows]
        self.walls, self.goals, boxes = set(), set(), set()
        self.player = None
        for y, row in enumerate(grid):
            for x, c in enumerate(row):
                i = y * self.w + x
                if c == "#":
                    self.walls.add(i)
                if c in ".*+":
                    self.goals.add(i)
                if c in "$*":
                    boxes.add(i)
                if c in "@+":
                    self.player = i
        if self.player is None or len(boxes) != len(self.goals) or not boxes:
            raise ValueError("malformed level")
        self.boxes = tuple(sorted(boxes))
        # Inside = reachable from the player without crossing walls.
        self.inside, stack = {self.player}, [self.player]
        while stack:
            i = stack.pop()
            for n in self.neighbours(i):
                if n not in self.walls and n not in self.inside:
                    self.inside.add(n)
                    stack.append(n)
        self.live = self.live_cells()
        self.goal_dist = self.pull_distances()

    def neighbours(self, i: int):
        x, y = i % self.w, i // self.w
        for dx, dy, _ in DIRS:
            nx, ny = x + dx, y + dy
            if 0 <= nx < self.w and 0 <= ny < self.h:
                yield ny * self.w + nx

    def step(self, i: int, dx: int, dy: int):
        x, y = i % self.w + dx, i // self.w + dy
        if 0 <= x < self.w and 0 <= y < self.h:
            return y * self.w + x
        return None

    def live_cells(self) -> set:
        """Cells from which a box can still be pushed onto some goal."""
        live, stack = set(self.goals), list(self.goals)
        while stack:
            i = stack.pop()
            for dx, dy, _ in DIRS:
                prev = self.step(i, dx, dy)  # box came from here
                keeper = self.step(i, 2 * dx, 2 * dy) if prev is not None else None
                if prev is None or keeper is None:
                    continue
                if prev in self.inside and keeper in self.inside and prev not in live:
                    live.add(prev)
                    stack.append(prev)
        return live

    def pull_distances(self) -> dict:
        """Per cell: fewest pushes to move a lone box from there to its nearest goal."""
        dist = {g: 0 for g in self.goals}
        frontier = list(self.goals)
        while frontier:
            nxt = []
            for i in frontier:
                for dx, dy, _ in DIRS:
                    prev = self.step(i, dx, dy)
                    keeper = self.step(i, 2 * dx, 2 * dy) if prev is not None else None
                    if prev is None or keeper is None or prev in dist:
                        continue
                    if prev in self.inside and keeper in self.inside:
                        dist[prev] = dist[i] + 1
                        nxt.append(prev)
            frontier = nxt
        return dist

    def encode(self) -> bytes:
        cells = []
        for i in range(self.w * self.h):
            # Boxes and goals walled off from the keeper still show.
            if i in self.walls:
                c = 1
            elif i == self.player:
                c = 7 if i in self.goals else 6
            elif i in self.boxes:
                c = 5 if i in self.goals else 4
            elif i in self.goals:
                c = 3
            else:
                c = 2 if i in self.inside else 0
            cells.append(c)
        if len(cells) % 2:
            cells.append(0)
        return bytes((cells[k] << 4) | cells[k + 1] for k in range(0, len(cells), 2))


def solve(level: Level, budget_s: float):
    """A* over push states. Returns (moves, pushes, path) or None."""
    goals = frozenset(level.goals)
    deadline = time.monotonic() + budget_s

    def heuristic(boxes):
        return sum(level.goal_dist[b] for b in boxes)

    def frozen_square(boxes, moved):
        # A 2x2 block of walls/boxes containing a box off-goal can never move.
        bs = set(boxes)
        for ox in (-1, 0):
            for oy in (-1, 0):
                corner = level.step(moved, ox, oy)
                if corner is None:
                    continue
                cells = [corner, level.step(corner, 1, 0), level.step(corner, 0, 1), level.step(corner, 1, 1)]
                if any(c is None for c in cells):
                    continue
                if all(c in level.walls or c in bs for c in cells) and any(c in bs and c not in goals for c in cells):
                    return True
        return False

    start = (level.boxes, level.player)
    best = {start: (0, 0)}
    parent = {start: None}
    heap = [(heuristic(level.boxes), 0, 0, level.boxes, level.player)]
    while heap:
        if time.monotonic() > deadline:
            return None
        _, moves, pushes, boxes, player = heapq.heappop(heap)
        if best.get((boxes, player), (1 << 30, 0)) < (moves, pushes):
            continue
        if goals.issuperset(boxes):
            path, state = [], (boxes, player)
            while parent[state]:
                prev, seg = parent[state]
                path.append(seg)
                state = prev
            return moves, pushes, "".join(reversed(path))
        box_set = set(boxes)
        # BFS the keeper's reachable area, remembering the walk to each cell.
        walk = {player: ""}
        queue = [player]
        for cell in queue:
            for dx, dy, ch in DIRS:
                n = level.step(cell, dx, dy)
                if n is None or n in level.walls or n in walk:
                    continue
                if n in box_set:
                    beyond = level.step(n, dx, dy)
                    if beyond is None or beyond in level.walls or beyond in box_set or beyond not in level.live:
                        continue
                    nb = tuple(sorted((box_set - {n}) | {beyond}))
                    if beyond not in goals and frozen_square(nb, beyond):
                        continue
                    seg = walk[cell] + ch.upper()
                    cost = (moves + len(seg), pushes + 1)
                    state = (nb, n)
                    if cost < best.get(state, (1 << 30, 0)):
                        best[state] = cost
                        parent[state] = ((boxes, player), seg)
                        heapq.heappush(heap, (cost[0] + heuristic(nb), cost[0], cost[1], nb, n))
                    continue
                walk[n] = walk[cell] + ch
                queue.append(n)
    return None


def replay(level: Level, path: str) -> bool:
    boxes, p = set(level.boxes), level.player
    for ch in path:
        dx, dy = {"u": (0, -1), "d": (0, 1), "l": (-1, 0), "r": (1, 0)}[ch.lower()]
        n = level.step(p, dx, dy)
        if n is None or n in level.walls:
            return False
        if n in boxes:
            b = level.step(n, dx, dy)
            if b is None or b in level.walls or b in boxes or not ch.isupper():
                return False
            boxes.remove(n)
            boxes.add(b)
        elif ch.isupper():
            return False
        p = n
    return boxes == level.goals


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="+", type=Path)
    ap.add_argument("--source", type=Path, help="local copy of the Microban text file")
    ap.add_argument("--solve-seconds", type=float, default=20.0, help="A* budget per level (0 skips par)")
    ap.add_argument("--solutions", type=Path, help="also write each level's par solution (LURD) here")
    ap.add_argument("--reuse", type=Path, help="take par from a previous --solutions file instead of searching")
    args = ap.parse_args()

    if args.source:
        text = args.source.read_text()
    else:
        with urllib.request.urlopen(SOURCE_URL) as resp:
            text = resp.read().decode("utf-8")
    levels = [Level(rows) for rows in parse_levels(text)]
    print(f"parsed {len(levels)} levels")

    known = {}
    if args.reuse:
        for line in args.reuse.read_text().splitlines():
            num, _, path = line.partition(" ")
            known[int(num)] = path.strip()

    records, solutions = [], []
    for n, lv in enumerate(levels, 1):
        if args.reuse:
            path = known.get(n, "")
            par = (len(path), sum(c.isupper() for c in path), path) if path else None
        else:
            par = solve(lv, args.solve_seconds) if args.solve_seconds > 0 else None
        if par and not replay(lv, par[2]):
            raise RuntimeError(f"level {n}: solver produced an invalid solution")
        moves, pushes, path = par if par else (0, 0, "")
        solutions.append(path)
        print(f"level {n:3d}: {lv.w}x{lv.h} boxes {len(lv.boxes):2d} par {moves or '-'}/{pushes or '-'}", flush=True)
        records.append(struct.pack("<BBHH", lv.w, lv.h, moves, pushes) + lv.encode())

    name = PACK_NAME.encode("utf-8")
    header = b"SKB1" + struct.pack("<HB", len(records), len(name)) + name
    offset = len(header) + 4 * len(records)
    index = bytearray()
    for rec in records:
        index += struct.pack("<I", offset)
        offset += len(rec)
    pack = header + bytes(index) + b"".join(records)

    for out in args.out:
        out.mkdir(parents=True, exist_ok=True)
        (out / "levels.bin").write_bytes(pack)
        (out / "NOTICE.txt").write_text(NOTICE)
        print(f"wrote {out / 'levels.bin'} ({len(pack)} bytes)")
    if args.solutions:
        args.solutions.write_text("".join(f"{i + 1} {s}\n" for i, s in enumerate(solutions)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
