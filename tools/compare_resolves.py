"""Compare the native renderer's resolves with the stock renderer's, pass by pass.

    compare_resolves.py <native prefix-f<frame>> <stock memory dump> [--out <dir>]

The native side comes from --dump_resolves (src/renderer/renderer.cpp): one
.res file per resolve of the frame, holding the destination's fetch constant
and the texels read back. The stock side is guest physical memory dumped on the
GPU timeline at the same game frame (SDK patch 8 with --dump_memory, run with
--readback_resolve=full so resolves land in memory). For each destination,
the native frame's last resolve is compared with what memory holds there,
untiled and byte-swapped as the fetch constant says. Depth (k_24_8_FLOAT) is
compared as floats. With --out, side-by-side images per destination.
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from ops_check import tiled_offset_2d  # noqa: E402

DXGI_R8G8B8A8_UNORM, DXGI_R32_FLOAT = 28, 41


def load_native(prefix):
    dumps = {}
    for path in sorted(glob.glob(prefix + '-*.res'), key=lambda p: int(p.rsplit('-', 1)[1][:-4])):
        raw = open(path, 'rb').read()
        fetch = struct.unpack_from('<6I', raw, 0)
        width, height, fmt, pitch = struct.unpack_from('<4I', raw, 24)
        rows = np.frombuffer(raw, np.uint8, pitch * height, 40).reshape(height, pitch)
        bpp = 4
        dumps[fetch[1] & ~0xFFF] = (fetch, fmt, rows[:, :width * bpp].reshape(height, width, bpp))
    return dumps


def swap(words, endian):
    if endian == 1:
        return ((words & 0x00FF00FF) << 8) | ((words >> 8) & 0x00FF00FF)
    if endian == 2:
        return words.byteswap()
    if endian == 3:
        return (words << 16) | (words >> 16)
    return words


def load_stock(memory, fetch):
    width, height = (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1
    base, endian, tiled = fetch[1] & ~0xFFF, (fetch[1] >> 6) & 3, fetch[0] >> 31
    pitch = ((fetch[0] >> 22) & 0x1FF) * 32 or width
    ys, xs = np.mgrid[0:height, 0:width]
    if tiled:
        offsets = np.vectorize(lambda x, y: tiled_offset_2d(x, y, pitch, 2))(xs, ys)
    else:
        offsets = (ys * pitch + xs) * 4
    words = np.frombuffer(memory, '<u4', count=len(memory) // 4)
    return swap(words[(base + offsets) // 4].astype(np.uint32), endian)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('native')
    parser.add_argument('memory')
    parser.add_argument('--out')
    args = parser.parse_args()
    memory = np.memmap(args.memory, np.uint8, 'r')
    for address, (fetch, fmt, texels) in load_native(args.native).items():
        stock = load_stock(memory, fetch)
        h, w = stock.shape
        if fmt == DXGI_R32_FLOAT:
            nat = texels.view('<f4')[:, :, 0]
            # k_24_8_FLOAT: 20e4 depth in the upper 24 bits.
            f24 = stock >> 8
            mant, exp = f24 & 0xFFFFF, f24 >> 20
            ref = np.where(exp == 0, mant / 2.0**20 * 2.0**-14, (1 + mant / 2.0**20) * 2.0**(exp.astype(np.int64) - 15))
            ref = np.where(f24 == 0, 0.0, ref)
            diff = np.abs(nat - ref)
            print(f'{address:08X} depth {w}x{h}: mean |d| {diff.mean():.6f}, max {diff.max():.6f}, '
                  f'native range {nat.min():.4f}-{nat.max():.4f}, stock {ref.min():.4f}-{ref.max():.4f}')
            continue
        # 8888: try the channel orders and keep the closest (the byte order is not known yet).
        bytes_ = stock.view(np.uint8).reshape(h, w, 4)
        best = None
        for order in ((0, 1, 2, 3), (3, 2, 1, 0), (1, 2, 3, 0), (2, 1, 0, 3), (3, 0, 1, 2), (2, 1, 0, 3)):
            ref = bytes_[:, :, order].astype(np.int16)
            d = np.abs(texels.astype(np.int16) - ref)
            score = d.mean()
            if best is None or score < best[0]:
                best = (score, order, ref, d)
        score, order, ref, d = best
        bad = (d[:, :, :3].max(axis=2) > 24).mean() * 100
        print(f'{address:08X} 8888 {w}x{h}: MAE {score:.2f} (order {order}), bad {bad:.2f}%, '
              f'mean native {texels.reshape(-1, 4).mean(0).round(1)} stock {ref.reshape(-1, 4).mean(0).round(1)}')
        if args.out:
            from PIL import Image
            os.makedirs(args.out, exist_ok=True)
            img = np.concatenate([texels[:, :, :3], ref[:, :, :3].astype(np.uint8),
                                  np.clip(d[:, :, :3].max(axis=2, keepdims=True).repeat(3, 2) * 4, 0, 255).astype(np.uint8)], 1)
            Image.fromarray(img).save(os.path.join(args.out, f'{address:08X}.png'))
            if texels.shape[2] == 4:
                alpha = np.concatenate([texels[:, :, 3], ref[:, :, 3].astype(np.uint8)], 1)
                Image.fromarray(alpha).save(os.path.join(args.out, f'{address:08X}-alpha.png'))


if __name__ == '__main__':
    main()
