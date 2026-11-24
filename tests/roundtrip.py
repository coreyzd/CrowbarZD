#!/usr/bin/env python3
# SPDX-License-Identifier: CC-BY-SA-3.0
"""Round-trip check: decompile a model, recompile it, decompile the result and compare.

usage: roundtrip.py <crowbarzd-cli> <studiomdl> <game folder> <model.mdl> [more.mdl ...]

The game folder needs a gameinfo.txt; recompiled models are written to a scratch
copy of it, never into the real game. Exit code 0 means every model matched.
"""
import math, os, re, shutil, subprocess, sys, tempfile

TOL = 0.05
QC_TOL = 0.02    # relative; QC numbers pass through float32 and compression
PHYS_TOL = 0.15  # collision hulls are stored in metres at lower precision  # units / radians; MDL animation data is quantised, so exact equality is not expected


def numbers(line):
    return [float(t) for t in line.split() if re.fullmatch(r"-?\d+(\.\d+)?", t)]


def split_smd(path):
    """Returns (lines before 'triangles', list of (material, [vertex number lists]))."""
    lines = open(path).read().splitlines()
    if "triangles" not in lines:
        return lines, []
    at = lines.index("triangles")
    body = [l for l in lines[at + 1:] if l != "end"]
    return lines[:at], [(body[i].strip(), [numbers(v) for v in body[i + 1:i + 4]]) for i in range(0, len(body) - 3, 4)]


def cloud_distance(pts, others, dims=8):
    """Largest distance from a point in pts to its nearest neighbour in others (capped search radius)."""
    cell = 0.25
    grid = {}
    for p in others:
        grid.setdefault(tuple(int(c // cell) for c in p[:3]), []).append(p)
    worst = 0.0
    for p in pts:
        key = tuple(int(c // cell) for c in p[:3])
        best = 1e9
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for o in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        best = min(best, max(abs(p[i] - o[i]) for i in range(dims)))
        worst = max(worst, best)
    return worst


def vta_frames(path):
    """Per 'time' block of a VTA file: the positions of the vertices that really move.

    The compiler drops vertices whose movement is negligible, so anything within 0.01 units of
    its base position is left out on both sides.
    """
    text = open(path).read()
    blocks = []
    for line in text[text.index("vertexanimation"):].splitlines()[1:]:
        if line.strip().startswith("time"):
            blocks.append({})
        elif blocks and line.strip() != "end":
            parts = line.split()
            blocks[-1][int(parts[0])] = tuple(float(v) for v in parts[1:4])
    base = blocks[0] if blocks else {}
    moved = lambda i, p: i not in base or max(abs(p[k] - base[i][k]) for k in range(3)) > 0.01
    return [list(blocks[0].values())] + [[p for i, p in block.items() if moved(i, p)] for block in blocks[1:]] if blocks else []


def compare_smd(a, b):
    (ha, ta), (hb, tb) = split_smd(a), split_smd(b)
    if a.endswith("_physics.smd"):
        # studiomdl rebuilds the convex hulls, so only the hull corner positions are compared.
        pa = [tuple(v[1:4]) for _, vs in ta for v in vs]
        pb = [tuple(v[1:4]) for _, vs in tb for v in vs]
        if not pa or not pb:
            return f"collision mesh is empty ({len(pa) // 3} vs {len(pb) // 3} triangles)"
        worst = max(cloud_distance(pa, pb, 3), cloud_distance(pb, pa, 3))
        return None if worst <= PHYS_TOL else f"largest collision vertex difference {worst:.4f} exceeds {PHYS_TOL}"
    if len(ha) != len(hb):
        return f"skeleton line count differs ({len(ha)} vs {len(hb)})"
    worst = 0.0
    for i, (x, y) in enumerate(zip(ha, hb)):
        nx, ny = numbers(x), numbers(y)
        if len(nx) != len(ny) or (not nx and x.strip() != y.strip()):
            return f"line {i + 1} differs: {x.strip()!r} vs {y.strip()!r}"
        # Skeleton lines are "bone x y z rx ry rz"; angles that differ by a full turn are the same.
        angle = lambda i: len(nx) == 7 and i >= 4
        worst = max([worst] + [min(abs(p - q), abs(abs(p - q) - 2 * math.pi)) if angle(i) else abs(p - q) for i, (p, q) in enumerate(zip(nx, ny))])
    if worst > TOL:
        return f"largest skeleton difference {worst:.4f} exceeds {TOL}"
    # studiomdl reorders triangles, so meshes are compared as sets: same triangle count
    # per material, and every vertex (position + normal + uv) has a twin on the other side.
    count = lambda tris: sorted((m, sum(1 for t in tris if t[0] == m)) for m in {t[0] for t in tris})
    ca, cb = count(ta), count(tb)
    # studiomdl drops degenerate triangles, so allow a difference of up to 0.5% per material.
    if [m for m, _ in ca] != [m for m, _ in cb] or any(abs(x - y) > max(1, x // 200) for (_, x), (_, y) in zip(ca, cb)):
        return f"triangle counts per material differ: {ca} vs {cb}"
    pa = [tuple(v[1:9]) for _, vs in ta for v in vs]
    pb = [tuple(v[1:9]) for _, vs in tb for v in vs]
    worst = max(cloud_distance(pa, pb), cloud_distance(pb, pa)) if pa else 0.0
    return None if worst <= TOL else f"largest vertex difference {worst:.4f} exceeds {TOL}"


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    cli, studiomdl, game = map(os.path.abspath, sys.argv[1:4])
    failed = 0
    for mdl in sys.argv[4:]:
        with tempfile.TemporaryDirectory() as tmp:
            scratch = os.path.join(tmp, "game")
            os.makedirs(scratch)
            shutil.copy(os.path.join(game, "gameinfo.txt"), scratch)
            first, second = os.path.join(tmp, "first"), os.path.join(tmp, "second")
            subprocess.run([cli, "decompile", mdl, "-o", first], check=True, stdout=subprocess.DEVNULL)
            name = os.path.splitext(os.path.basename(mdl))[0]
            qc = os.path.join(first, name + ".qc")
            run = subprocess.run([cli, "compile", qc, "-game", scratch, "-studiomdl", studiomdl], capture_output=True, text=True)
            modelname = re.search(r'\$modelname\s+"([^"]+)"', open(qc).read()).group(1)
            rebuilt = os.path.join(scratch, "models", modelname)
            if run.returncode != 0 or not os.path.exists(rebuilt):
                print(f"FAIL {name}: recompile failed\n{run.stdout[-1500:]}")
                failed += 1
                continue
            subprocess.run([cli, "decompile", rebuilt, "-o", second], check=True, stdout=subprocess.DEVNULL)
            problems = []
            for root, _, files in os.walk(first):
                for f in sorted(files):
                    if not f.endswith(".smd"):
                        continue
                    rel = os.path.relpath(os.path.join(root, f), first)
                    other = os.path.join(second, rel)
                    why = compare_smd(os.path.join(root, f), other) if os.path.exists(other) else "missing after recompile"
                    if why:
                        problems.append(f"  {rel}: {why}")
            # Flex shapes: every frame of the VTA must move the same vertices to the same places.
            for vta in sorted(f for f in os.listdir(first) if f.endswith(".vta")):
                other = os.path.join(second, vta)
                if not os.path.exists(other):
                    problems.append(f"  {vta}: missing after recompile")
                    continue
                fa, fb = vta_frames(os.path.join(first, vta)), vta_frames(other)
                if len(fa) != len(fb):
                    problems.append(f"  {vta}: {len(fa)} flex frames became {len(fb)}")
                    continue
                for i, (pa, pb) in enumerate(zip(fa, fb)):
                    if i == 0:
                        continue  # the base shape is the reference mesh, compared above
                    if not pa or not pb or abs(len(pa) - len(pb)) > max(2, len(pa) // 20):
                        problems.append(f"  {vta}: frame {i} moves {len(pa)} vertices before and {len(pb)} after")
                        break
                    worst = max(cloud_distance(pa, pb, 3), cloud_distance(pb, pa, 3))
                    if worst > TOL:
                        problems.append(f"  {vta}: frame {i} differs by {worst:.4f}")
                        break
            # Counts of bones, animations, IK rules and so on must survive.
            info = lambda f: [l for l in subprocess.run([cli, "info", f], capture_output=True, text=True).stdout.splitlines()
                              if not l.startswith(("name:", "version:", "flags:", "materials:"))]  # unused materials are dropped by the compiler
            ia, ib = info(mdl), info(rebuilt)
            if ia != ib:
                problems.append("  model summary differs: " + "; ".join(f"{x} -> {y.split(': ')[-1]}" for x, y in zip(ia, ib) if x != y))
            # The QC written from the rebuilt model should say the same things as the first one.
            def qc_lines(path):
                # The compiler may store ragdoll parts in another order, so per-joint lines are sorted.
                lines = open(path).read().splitlines()
                joints = sorted(l for l in lines if l.startswith("\t$joint"))
                return [l for l in lines if not l.startswith("\t$joint")] + joints
            qa, qb = qc_lines(qc), qc_lines(os.path.join(second, name + ".qc"))
            # Procedural bone files are plain text too; treat them as part of the QC.
            for vrd in sorted(f for f in os.listdir(first) if f.endswith(".vrd")):
                qa += open(os.path.join(first, vrd)).read().splitlines()
                qb += open(os.path.join(second, vrd)).read().splitlines() if os.path.exists(os.path.join(second, vrd)) else ["(no VRD file)"]
            diffs = []
            if len(qa) != len(qb):
                import difflib
                diffs = [l for l in difflib.unified_diff(qa, qb, lineterm="", n=0) if l[:1] in "+-" and l[:3] not in ("+++", "---")]
            else:
                for x, y in zip(qa, qb):
                    wx, wy = x.split(), y.split()
                    same = len(wx) == len(wy) and all(
                        a == b or (re.fullmatch(r"-?\d+(\.\d+)?", a.strip('"')) and re.fullmatch(r"-?\d+(\.\d+)?", b.strip('"'))
                                   and (abs(float(a.strip('"')) - float(b.strip('"'))) <= QC_TOL * max(1.0, abs(float(a.strip('"'))))
                                        or (x.startswith("<trigger>") and abs(abs(float(a) - float(b)) - 360) < 0.01)))  # -180 and 180 degrees
                        for a, b in zip(wx, wy))
                    if not same:
                        diffs += ["-" + x, "+" + y]
            if diffs:
                problems.append(f"  {name}.qc differs after the round trip ({len(diffs)} lines):\n" + "\n".join("    " + d for d in diffs[:12]))
            print(("FAIL " if problems else "ok   ") + name + ("\n" + "\n".join(problems) if problems else ""))
            failed += bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
