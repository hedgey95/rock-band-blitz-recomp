"""Contact sheets of the textures written by --dump_textures (native renderer N4).

    texture_sheet.py <dump dir> <output prefix> [--per-page N] [--size PX]
    texture_sheet.py <dump dir> --check-mips

Decodes level 0 of each DDS (BC1, BC2, BC3, BC5, RGBA8, RGBA32F; the first
face of cube maps), applies the fetch constant swizzle from the file name
(_swXXX: 3 bits per output component xyzw; 0-3 pick RGBA, 4 is 0, 5 is 1),
and writes <prefix>-<page>.png grids with the file name under each texture.
Alpha is shown over a checkerboard.

--check-mips tests the guest layout math instead: every mip level (including
the packed mip tail) must look like level 0 scaled down. Wrong mip addresses
or packed-tail coordinates give unrelated blocks and a large error.
"""
import glob
import os
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

DXGI = {71: 'BC1', 74: 'BC2', 77: 'BC3', 83: 'BC5', 28: 'RGBA8', 2: 'RGBA32F'}


def rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def bc1_colors(blocks, force_four):
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1) | force_four
    palette = np.zeros((len(blocks), 4, 4), np.int32)
    palette[:, 0, :3], palette[:, 1, :3] = p0, p1
    palette[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    palette[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    palette[:, :, 3] = 255
    palette[:, 3, 3] = np.where(four, 255, 0)
    bits = blocks[:, 4:8].astype(np.uint32)
    idx = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    sel = (idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    return np.take_along_axis(palette, sel[:, :, None].astype(np.int64).repeat(4, 2), 1)  # (n, 16, 4)


def bc4(blocks):
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    pal = np.zeros((len(blocks), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    eight = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(eight, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = (bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.int64), 1)  # (n, 16)


def decode(kind, data, w, h):
    if kind in ('RGBA8', 'RGBA32F'):
        if kind == 'RGBA8':
            img = np.frombuffer(data, np.uint8, w * h * 4).reshape(h, w, 4).astype(np.int32)
        else:
            img = (np.clip(np.frombuffer(data, np.float32, w * h * 4), 0, 1) * 255).reshape(h, w, 4)
        return img.astype(np.uint8)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    size = 8 if kind == 'BC1' else 16
    blocks = np.frombuffer(data, np.uint8, bw * bh * size).reshape(-1, size)
    if kind == 'BC1':
        px = bc1_colors(blocks, np.zeros(len(blocks), bool))
    elif kind == 'BC3':
        px = bc1_colors(blocks[:, 8:], np.ones(len(blocks), bool))
        px[:, :, 3] = bc4(blocks[:, :8])
    elif kind == 'BC2':
        px = bc1_colors(blocks[:, 8:], np.ones(len(blocks), bool))
        nib = np.unpackbits(blocks[:, :8], axis=1, bitorder='little').reshape(-1, 16, 4)
        px[:, :, 3] = (nib * (1 << np.arange(4))).sum(-1) * 17
    else:  # BC5: red and green
        px = np.zeros((len(blocks), 16, 4), np.int32)
        px[:, :, 0], px[:, :, 1], px[:, :, 3] = bc4(blocks[:, :8]), bc4(blocks[:, 8:]), 255
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


def levels(path):
    """All mip levels of the first face, decoded, without the swizzle."""
    data = open(path, 'rb').read()
    h, w = struct.unpack_from('<II', data, 12)
    mips = struct.unpack_from('<I', data, 28)[0]
    kind = DXGI.get(struct.unpack_from('<I', data, 128)[0])
    out, offset = [], 148
    for level in range(mips):
        lw, lh = max(w >> level, 1), max(h >> level, 1)
        if kind in ('RGBA8', 'RGBA32F'):
            size = lw * lh * (4 if kind == 'RGBA8' else 16)
        else:
            size = ((lw + 3) // 4) * ((lh + 3) // 4) * (8 if kind == 'BC1' else 16)
        out.append(decode(kind, data[offset:offset + size], lw, lh).astype(np.float32))
        offset += size
    return kind, out


def check_mips(src):
    worst = []
    checked = levels_checked = 0
    for path in sorted(glob.glob(os.path.join(src, '*.dds'))):
        kind, lv = levels(path)
        if kind is None or len(lv) < 2 or lv[0].std() < 1:  # no mips, or flat (empty) texture
            continue
        checked += 1
        img = lv[0]
        for level in range(1, len(lv)):
            h, w = lv[level].shape[:2]
            if h < 4 or w < 4:
                break
            # Box-filter level 0 down to this level's size.
            fy, fx = img.shape[0] // h, img.shape[1] // w
            ref = img[:h * fy, :w * fx].reshape(h, fy, w, fx, 4).mean((1, 3))
            err = np.abs(ref - lv[level]).mean()
            spread = np.abs(ref - ref.mean((0, 1))).mean() + 1e-3
            levels_checked += 1
            worst.append((err / spread, err, os.path.basename(path), level, w, h))
    # Bad: the error is large in absolute terms and against the image's own
    # variation (nearly flat levels, like normal maps, have tiny variation).
    bad = [x for x in worst if x[0] > 0.5 and x[1] > 8]
    worst.sort(key=lambda x: -x[1])
    print(f'{checked} textures with mips, {levels_checked} levels of 4x4 or more compared with level 0 scaled down')
    print(f'levels off by more than 8/255 and half the image variation: {len(bad)}; largest errors:')
    for ratio, err, name, level, w, h in worst[:6]:
        print(f'  {ratio:5.2f} (mean error {err:5.1f}/255) {name} level {level} ({w}x{h})')
    return not bad


def load(path):
    data = open(path, 'rb').read()
    h, w = struct.unpack_from('<II', data, 12)
    fmt = struct.unpack_from('<I', data, 128)[0]
    kind = DXGI.get(fmt)
    if not kind:
        return None, f'dxgi {fmt}'
    img = decode(kind, data[148:], w, h)
    m = re.search(r'_sw([0-9A-F]{3})', os.path.basename(path))
    if m:
        sw = int(m.group(1), 16)
        src = np.concatenate([img, np.zeros_like(img[..., :1]), np.full_like(img[..., :1], 255)], -1)
        img = np.stack([src[..., (sw >> (3 * i)) & 7] if ((sw >> (3 * i)) & 7) < 6 else src[..., 4]
                        for i in range(4)], -1)
    return img, kind


def main():
    args = sys.argv[1:]
    if '--check-mips' in args:
        sys.exit(0 if check_mips(args[0]) else 1)
    per_page = int(args[args.index('--per-page') + 1]) if '--per-page' in args else 48
    size = int(args[args.index('--size') + 1]) if '--size' in args else 160
    src, prefix = args[0], args[1]
    files = sorted(glob.glob(os.path.join(src, '*.dds')))
    cols = 8
    for page in range(0, len(files), per_page):
        chunk = files[page:page + per_page]
        rows = (len(chunk) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * (size + 8), rows * (size + 24)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for i, path in enumerate(chunk):
            x, y = (i % cols) * (size + 8) + 4, (i // cols) * (size + 24) + 4
            img, kind = load(path)
            name = os.path.basename(path)[:-4]
            if img is not None:
                im = Image.fromarray(img, 'RGBA')
                im.thumbnail((size, size))
                checker = Image.new('RGB', im.size, (200, 200, 200))
                cd = ImageDraw.Draw(checker)
                for cy in range(0, im.size[1], 8):
                    for cx in range((cy // 8) % 2 * 8, im.size[0], 16):
                        cd.rectangle([cx, cy, cx + 7, cy + 7], fill=(150, 150, 150))
                checker.paste(im, (0, 0), im)
                sheet.paste(checker, (x, y))
            draw.text((x, y + size + 2), f'{name[:9]} {name.split("_")[1]} {kind}', fill=(230, 230, 230))
        out = f'{prefix}-{page // per_page + 1}.png'
        sheet.save(out)
        print(out, len(chunk))


if __name__ == '__main__':
    main()
