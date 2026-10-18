"""Checks D3D resolves decoded from the game's calls against Xenia's registers.

    ops_check.py <cp.bin> <ops.bin>

ops.bin comes from --trace_ops (src/renderer/d3d_ops.cpp): each D3DDevice_Resolve
and D3DDevice_ClearF call with its decoded parameters and the command buffer
range it wrote. cp.bin comes from --trace_draws (SDK patch 6). Each op is
paired with Xenia's draw whose packet lies in that range, and the resolve's
destination and flags are compared with the copy registers Xenia decoded.
"""
import collections
import struct
import sys

import numpy as np

import device_map as dm

RESOLVE = 0x8250C938
CLEAR = 0x82519F50


def tiled_offset_2d(x, y, pitch, bpb_log2):
    """Xenos 2D tiled byte offset (texture_util::GetTiledOffset2D)."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def load_ops(path):
    raw = open(path, 'rb').read()
    ops, pos = [], 0
    while pos + 32 <= len(raw):
        magic, frame, func, before, after, s0, s1, s2 = struct.unpack_from('<8I', raw, pos)
        assert magic == 0x5352504F
        pos += 32
        if func == RESOLVE:
            b = raw[pos:pos + 96]
            pos += 96
            flags, source = struct.unpack_from('<II', b, 0)
            clear_color, clear_depth, has_rect = b[8], b[9], b[10]
            rect = struct.unpack_from('<4i', b, 12)
            dest_texture = struct.unpack_from('<I', b, 28)[0]
            fetch = struct.unpack_from('<6I', b, 32)
            point = struct.unpack_from('<2i', b, 56)
            level, slice_ = struct.unpack_from('<II', b, 64)
            color = struct.unpack_from('<4f', b, 72)
            z, stencil = struct.unpack_from('<fI', b, 88)
            op = dict(kind='resolve', flags=flags, source=source, clear_color=clear_color,
                      clear_depth=clear_depth, rect=rect if has_rect else None, dest=dest_texture,
                      fetch=fetch, point=point, level=level, slice=slice_, color=color, z=z, stencil=stencil)
        else:
            b = raw[pos:pos + 48]
            pos += 48
            flags = struct.unpack_from('<I', b, 0)[0]
            op = dict(kind='clear', flags=flags, rect=struct.unpack_from('<4i', b, 8) if b[4] else None,
                      color=struct.unpack_from('<4f', b, 24), z=struct.unpack_from('<f', b, 40)[0],
                      stencil=struct.unpack_from('<I', b, 44)[0])
        op.update(frame=frame, lo=before & 0x1FFFFFFF, hi=after & 0x1FFFFFFF, surface=(s0, s1, s2))
        ops.append(op)
    return ops


def main():
    cp = np.fromfile(sys.argv[1], dtype='<u4')
    cp = cp[:len(cp) // dm.CPN * dm.CPN].reshape(-1, dm.CPN)
    ops = load_ops(sys.argv[2])
    reg = lambda rec, r: int(rec[dm.reg_index(r)])
    print(f'{len(ops)} ops:', dict(collections.Counter(o['kind'] for o in ops)))
    checks = collections.Counter()
    failures = []
    for op in ops:
        lo, hi = op['lo'], op['hi']
        if hi < lo:
            lo = hi - 0x10000
        draws = [rec for rec in cp if int(rec[1]) == op['frame'] and lo < int(rec[2]) <= hi]
        if not draws:
            checks['no Xenia draw in range'] += 1
            continue
        if op['kind'] != 'resolve':
            # ClearF draws a rect with D3D's built-in shaders; where its color
            # reaches the GPU is not decoded yet, so clears are only paired here.
            checks['clears paired with their draw'] += 1
            continue
        # Predicated tiling: one Resolve call writes a copy per tile; the first
        # one starts at the texture base, later ones at the tile's offset.
        copies = [rec for rec in draws if reg(rec, 0x2208) & 7 == 6]
        checks[f'copies per resolve: {len(copies)}'] += 1
        rec = copies[0] if copies else draws[-1]
        mode = reg(rec, 0x2208) & 7
        ctl, base, pitch, info = (reg(rec, r) for r in (0x2318, 0x2319, 0x231A, 0x231B))
        fetch = op['fetch']
        tex_base = fetch[1] & 0xFFFFF000
        tex_w, tex_h = (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1
        # Predicated tiling resolves each tile separately: the copy lands at the
        # tile's origin, which is minus PA_SC_WINDOW_OFFSET.
        wo = reg(rec, 0x2080)
        tile_x = -((wo & 0x7FFF) - (0x8000 if wo & 0x4000 else 0))
        tile_y = -(((wo >> 16) & 0x7FFF) - (0x8000 if wo & 0x40000000 else 0))
        if tile_x or tile_y:
            checks['resolves of a second tile'] += 1
        tile_offset = tiled_offset_2d(tile_x, tile_y, pitch & 0x3FFF, 2) if (tile_x or tile_y) else 0
        expect = {
            'EDRAM mode is copy': mode == 6,
            'source select': (ctl & 7) == op['source'],
            'clear color flag': bool((ctl >> 8) & 1) == bool(op['clear_color']),
            'clear depth flag': bool((ctl >> 9) & 1) == bool(op['clear_depth']),
            'dest base = texture base (+ tile origin)': (base & 0x1FFFFFFF) == ((tex_base + tile_offset) & 0x1FFFFFFF),
            # Depth resolves copy the raw 24:8 words as 32 bits per pixel (k_8_8_8_8).
            'dest format = texture format': ((info >> 7) & 0x3F) == (
                6 if op['source'] == 4 and (fetch[1] & 0x3F) in (22, 23) else fetch[1] & 0x3F),
            'dest height = texture height': ((pitch >> 16) & 0x3FFF) == tex_h,
            'dest pitch >= texture width': (pitch & 0x3FFF) >= tex_w,
        }
        for name, ok in expect.items():
            checks[name + (': ok' if ok else ': MISMATCH')] += 1
            if not ok and len(failures) < 8:
                failures.append((name, hex(op['flags']), hex(ctl), hex(base), hex(pitch), hex(info),
                                 hex(tex_base), f'{tex_w}x{tex_h}', fetch[1] & 0x3F))
    for k, v in sorted(checks.items()):
        print(f'  {v:4}  {k}')
    for f in failures:
        print('  example:', f)
    sys.exit(1 if any('MISMATCH' in k or 'no Xenia' in k or ': NO' in k for k in checks) else 0)


if __name__ == '__main__':
    main()
