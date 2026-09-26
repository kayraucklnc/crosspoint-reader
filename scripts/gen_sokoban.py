#!/usr/bin/env python3
"""Build the Sokoban game's SD-card level pack from David W. Skinner's Microban.

Source: Microban (155 puzzles, revised April 2000) by David W. Skinner. His
sets are freely redistributed by Sokoban programs with credit to the author;
this pack is built for personal use. The text copy used here is the Let's
Logic export mirrored at SOURCE_URL. The converted pack is NOT committed; run
this script to generate it (the par search takes a few minutes):

    python3 scripts/gen_sokoban.py OUT_DIR [OUT_DIR ...] [--source FILE] [--festival FILE]
                                   [--solve-seconds N] [--solutions FILE | --reuse FILE]

e.g. OUT_DIR = fs_/.crosspoint/games/sokoban (simulator) or
<sd-extra>/.crosspoint/games/sokoban.

Output: levels.bin + NOTICE.txt. levels.bin layout (little endian):
  "SKB2", u16 levelCount, u8 nameLength, name (UTF-8),
  u32 offset[levelCount] (from file start), then per level:
  u8 width, u8 height, u16 parMoves, u16 parPushes (0 = unknown),
  ceil(width * height / 2) bytes of 4-bit cells, row-major, high nibble first,
  u16 solutionMoves, ceil(solutionMoves / 4) bytes of 2-bit moves
  (0 up, 1 right, 2 down, 3 left), first move in the high bits.
Cell codes: 0 outside, 1 wall, 2 floor, 3 goal, 4 box, 5 box on goal,
6 player, 7 player on goal. ("SKB1" packs are the same without solutions.)

Every level ships a solution, replayed here before it is written; the game
uses it for hints. Par is the move-optimal length from an A* search (pushes
break ties). Levels that search cannot finish in its time budget take the
shortest verified solution from a weighted best-first search or from the
Festival 3.1 solver's published Microban solutions (FESTIVAL_URL), and keep
par 0. A level with no verified solution fails the build.
"""

import argparse
import heapq
import re
import struct
import sys
import time
import urllib.request
from pathlib import Path

SOURCE_URL = ("https://raw.githubusercontent.com/danzinho007/Sokoban/master/"
              "Formato%20txt/Microban%2001%20(6).txt")
FESTIVAL_URL = ("https://raw.githubusercontent.com/spassvogel/retroban/master/"
                "random/%5Bx%5D%20microban.txt")
PACK_NAME = "Microban"
NOTICE = """Sokoban levels: Microban (155 puzzles, revised April 2000)
by David W. Skinner. Redistributed with credit to the author, for personal use.
Par values were computed by scripts/gen_sokoban.py (move-optimal A* search).
Hint solutions for levels without par: own search or Festival 3.1 (Yaron Shoham).
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

    def pull_distances_from(self, goal: int) -> dict:
        """Per cell: fewest pushes to move a lone box from there onto `goal`."""
        dist = {goal: 0}
        frontier = [goal]
        while frontier:
            nxt = []
            for i in frontier:
                for dx, dy, _ in DIRS:
                    prev = self.step(i, dx, dy)
                    keeper = self.step(i, 2 * dx, 2 * dy) if prev is not None else None
                    if prev is None or keeper is None or prev in dist:
                        continue
                    if prev in self.inside and keeper in self.inside and prev not in self.walls \
                            and keeper not in self.walls:
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


def solve_greedy(level: Level, budget_s: float, weight: int = 4):
    """Weighted best-first over push states with a normalized keeper.

    Much faster than solve() on the crowded levels, but the result is only a
    valid solution, not a move-optimal one. Returns (moves, pushes, path) or None.
    """
    goals = frozenset(level.goals)
    deadline = time.monotonic() + budget_s
    goal_list = sorted(goals)
    per_goal = {g: level.pull_distances_from(g) for g in goal_list}

    def heuristic(boxes):
        # Greedy box-goal matching on pull distances: a tighter bound than
        # summing each box's nearest goal when several boxes share one goal.
        pairs = sorted((per_goal[g].get(b, 999), b, g) for b in boxes for g in goal_list)
        used_b, used_g, total = set(), set(), 0
        for d, b, g in pairs:
            if b in used_b or g in used_g:
                continue
            used_b.add(b)
            used_g.add(g)
            total += d
        return total

    def reach(boxes, player):
        walk = {player: ""}
        queue = [player]
        for cell in queue:
            for dx, dy, ch in DIRS:
                n = level.step(cell, dx, dy)
                if n is None or n in level.walls or n in walk or n in boxes:
                    continue
                walk[n] = walk[cell] + ch
                queue.append(n)
        return walk

    def frozen(boxes, moved):
        for ox in (-1, 0):
            for oy in (-1, 0):
                corner = level.step(moved, ox, oy)
                if corner is None:
                    continue
                cells = [corner, level.step(corner, 1, 0), level.step(corner, 0, 1), level.step(corner, 1, 1)]
                if any(c is None for c in cells):
                    continue
                if all(c in level.walls or c in boxes for c in cells) and any(
                        c in boxes and c not in goals for c in cells):
                    return True
        return False

    start_boxes = frozenset(level.boxes)
    start_walk = reach(start_boxes, level.player)
    start_key = (start_boxes, min(start_walk))
    parent = {start_key: None}
    g_cost = {start_key: 0}
    counter = 0
    heap = [(weight * heuristic(start_boxes), counter, start_boxes, level.player)]
    while heap:
        if time.monotonic() > deadline:
            return None
        _, _, boxes, player = heapq.heappop(heap)
        walk = reach(boxes, player)
        key = (boxes, min(walk))
        if goals.issuperset(boxes):
            segs = []
            while parent[key]:
                key, seg = parent[key]
                segs.append(seg)
            path = "".join(reversed(segs))
            return len(path), sum(c.isupper() for c in path), path
        g = g_cost[key]
        for cell, route in walk.items():
            for dx, dy, ch in DIRS:
                box = level.step(cell, dx, dy)
                if box not in boxes:
                    continue
                beyond = level.step(box, dx, dy)
                if beyond is None or beyond in level.walls or beyond in boxes or beyond not in level.live:
                    continue
                nb = (boxes - {box}) | {beyond}
                if beyond not in goals and frozen(nb, beyond):
                    continue
                nkey = (nb, min(reach(nb, box)))
                if nkey in g_cost:
                    continue
                g_cost[nkey] = g + 1
                parent[nkey] = (key, route + ch.upper())
                counter += 1
                heapq.heappush(heap, (g + 1 + weight * heuristic(nb), counter, nb, box))
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


def festival_solutions(text: str) -> list:
    """Solutions in file order from a Festival solution file (one per level)."""
    return re.findall(r"\nSolution\n([lurdLURD]+)\n", text)


def pack_moves(path: str) -> bytes:
    codes = ["urdl".index(c.lower()) for c in path]
    codes += [0] * (-len(codes) % 4)
    body = bytes((codes[k] << 6) | (codes[k + 1] << 4) | (codes[k + 2] << 2) | codes[k + 3]
                 for k in range(0, len(codes), 4))
    return struct.pack("<H", len(path)) + body


def read_text(path, url: str) -> str:
    if path:
        return path.read_text()
    with urllib.request.urlopen(url) as resp:
        return resp.read().decode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="+", type=Path)
    ap.add_argument("--source", type=Path, help="local copy of the Microban text file")
    ap.add_argument("--festival", type=Path, help="local copy of the Festival solution file")
    ap.add_argument("--solve-seconds", type=float, default=20.0, help="A* budget per level")
    ap.add_argument("--solutions", type=Path, help="also write every level's solution here (~ = not optimal)")
    ap.add_argument("--reuse", type=Path, help="take solutions from a previous --solutions file instead of searching")
    args = ap.parse_args()

    levels = [Level(rows) for rows in parse_levels(read_text(args.source, SOURCE_URL))]
    print(f"parsed {len(levels)} levels")

    known = {}
    if args.reuse:
        for line in args.reuse.read_text().splitlines():
            parts = line.split()
            known[int(parts[0])] = (parts[1], len(parts) < 3)
    festival = None

    records, solutions = [], []
    for n, lv in enumerate(levels, 1):
        if n in known:
            path, optimal = known[n]
        else:
            found = solve(lv, args.solve_seconds)
            path, optimal = (found[2], True) if found else ("", False)
            if not found:
                if festival is None:
                    festival = festival_solutions(read_text(args.festival, FESTIVAL_URL))
                greedy = solve_greedy(lv, args.solve_seconds)
                candidates = [p for p in ([greedy[2]] if greedy else []) + festival[n - 1:n] if replay(lv, p)]
                path = min(candidates, key=len) if candidates else ""
        if not path or not replay(lv, path):
            raise RuntimeError(f"level {n}: no verified solution")
        pushes = sum(c.isupper() for c in path)
        moves, par_pushes = (len(path), pushes) if optimal else (0, 0)
        solutions.append((path, optimal))
        print(f"level {n:3d}: {lv.w}x{lv.h} boxes {len(lv.boxes):2d} "
              f"solution {len(path)}/{pushes}{'' if optimal else ' (not optimal)'}", flush=True)
        records.append(struct.pack("<BBHH", lv.w, lv.h, moves, par_pushes) + lv.encode() + pack_moves(path))

    name = PACK_NAME.encode("utf-8")
    header = b"SKB2" + struct.pack("<HB", len(records), len(name)) + name
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
        args.solutions.write_text(
            "".join(f"{i + 1} {p}{'' if opt else ' ~'}\n" for i, (p, opt) in enumerate(solutions)))
    print(f"all {len(levels)} levels solvable (every solution replay-verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
