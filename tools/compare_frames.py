"""Compare the native renderer's frames with the stock renderer's, frame by frame.

    compare_frames.py <reference prefix> <native prefix> [--diff-dir <dir>] [--max-bad <fraction>]
                      [--search <frames>] [--ref-start <frame> --native-start <frame>]

Reads <prefix>-f<N>.png (written by --screenshot_frames and run-scripted.ps1) for
every N both sides have. Per frame: mean absolute error (0-255, all channels),
PSNR, and the fraction of pixels whose largest channel difference exceeds 24/255
("bad" pixels: big enough to see, small enough to ignore filtering and
rounding). With --diff-dir, writes an amplified difference image per frame.
Exit status 1 if any frame's bad fraction is above --max-bad (default 0.01).

With --search K, each native frame N is compared with the reference frame in
[N-K, N+K] that matches it best (lowest MAE), and the offset is printed: song
time follows the audio clock, so gameplay can be shifted by a constant number
of frames between runs (load times differ).

With --ref-start and --native-start (each run's first busy frame, logged as
"screenshot_busy_frames: first busy frame N"), frames are paired by their
distance from those starts instead (captures from --screenshot_busy_frames).
"""
import argparse
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

BAD_LEVEL = 24


def frames(prefix):
    out = {}
    for path in glob.glob(glob.escape(prefix) + '-f*.png'):
        m = re.search(r'-f(\d+)\.png$', path)
        if m:
            out[int(m.group(1))] = path
    return out


def compare(a_path, b_path):
    a = np.asarray(Image.open(a_path).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int16)
    if a.shape != b.shape:
        return None, None
    diff = np.abs(a - b)
    mae = float(diff.mean())
    mse = float((diff.astype(np.float64) ** 2).mean())
    psnr = float('inf') if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    bad = float((diff.max(axis=2) > BAD_LEVEL).mean())
    return dict(mae=mae, psnr=psnr, bad=bad), diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('reference')
    parser.add_argument('native')
    parser.add_argument('--diff-dir')
    parser.add_argument('--max-bad', type=float, default=0.01)
    parser.add_argument('--search', type=int, default=0)
    parser.add_argument('--ref-start', type=int, default=0)
    parser.add_argument('--native-start', type=int, default=0)
    args = parser.parse_args()
    ref, nat = frames(args.reference), frames(args.native)
    pairs = {}
    shift = args.ref_start - args.native_start
    for n in sorted(nat):
        candidates = [m for m in ref if abs(m - (n + shift)) <= args.search]
        if not candidates:
            continue
        if len(candidates) == 1:
            pairs[n] = candidates[0]
        else:
            pairs[n] = min(candidates, key=lambda m: compare(ref[m], nat[n])[0]['mae'])
    common = sorted(pairs)
    if not common:
        print('no frames in common')
        return 1
    failed = 0
    print(f'{"frame":>6} {"ref":>6} {"MAE":>6} {"PSNR dB":>8} {"bad px":>8}')
    for n in common:
        stats, diff = compare(ref[pairs[n]], nat[n])
        if stats is None:
            print(f'{n:>6} {pairs[n]:>6} size mismatch')
            failed += 1
            continue
        ok = stats['bad'] <= args.max_bad
        failed += not ok
        print(f'{n:>6} {pairs[n]:>6} {stats["mae"]:6.2f} {stats["psnr"]:8.2f} {100 * stats["bad"]:7.2f}% '
              f'{"" if ok else "FAIL"}')
        if args.diff_dir:
            os.makedirs(args.diff_dir, exist_ok=True)
            image = np.clip(diff.max(axis=2) * 4, 0, 255).astype(np.uint8)
            Image.fromarray(image).save(os.path.join(args.diff_dir, f'diff-f{n}.png'))
    missing = sorted(set(nat) - set(pairs))
    if missing:
        print('no reference frame for:', missing)
    print(f'{len(common) - failed}/{len(common)} frames within {100 * args.max_bad:g}% bad pixels')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
