"""Inventory of the GPU resources the game uses, from a --trace_draws/--trace_device capture.

    resource_inventory.py <cp.bin> <dev.bin>

Per draw (Xenia's registers, see tools/device_map.py): textures in the fetch
constants, vertex streams, render targets, EDRAM mode, and resolve (copy) and
clear parameters, grouped by the D3D call that issued the draw.
"""
import collections
import sys

import numpy as np

import device_map as dm

TEXTURE_FORMATS = (
    'k_1_REVERSE k_1 k_8 k_1_5_5_5 k_5_6_5 k_6_5_5 k_8_8_8_8 k_2_10_10_10 k_8_A k_8_B k_8_8 '
    'k_Cr_Y1_Cb_Y0_REP k_Y1_Cr_Y0_Cb_REP k_16_16_EDRAM k_8_8_8_8_A k_4_4_4_4 k_10_11_11 k_11_11_10 '
    'k_DXT1 k_DXT2_3 k_DXT4_5 k_16_16_16_16_EDRAM k_24_8 k_24_8_FLOAT k_16 k_16_16 k_16_16_16_16 '
    'k_16_EXPAND k_16_16_EXPAND k_16_16_16_16_EXPAND k_16_FLOAT k_16_16_FLOAT k_16_16_16_16_FLOAT '
    'k_32 k_32_32 k_32_32_32_32 k_32_FLOAT k_32_32_FLOAT k_32_32_32_32_FLOAT k_32_AS_8 k_32_AS_8_8 '
    'k_16_MPEG k_16_16_MPEG k_8_INTERLACED k_32_AS_8_INTERLACED k_32_AS_8_8_INTERLACED k_16_INTERLACED '
    'k_16_MPEG_INTERLACED k_16_16_MPEG_INTERLACED k_DXN k_8_8_8_8_AS_16_16_16_16 k_DXT1_AS_16_16_16_16 '
    'k_DXT2_3_AS_16_16_16_16 k_DXT4_5_AS_16_16_16_16 k_2_10_10_10_AS_16_16_16_16 k_10_11_11_AS_16_16_16_16 '
    'k_11_11_10_AS_16_16_16_16 k_32_32_32_FLOAT k_DXT3A k_DXT5A k_CTX1 k_DXT3A_AS_1_1_1_1 '
    'k_8_8_8_8_GAMMA_EDRAM k_2_10_10_10_FLOAT_EDRAM').split()
COLOR_FORMATS = ['k_8_8_8_8', 'k_8_8_8_8_GAMMA', 'k_2_10_10_10', 'k_2_10_10_10_FLOAT', 'k_16_16',
                 'k_16_16_16_16', 'k_16_16_FLOAT', 'k_16_16_16_16_FLOAT', '8?', '9?', 'k_2_10_10_10_AS_10_10_10_10',
                 '11?', 'k_2_10_10_10_FLOAT_AS_16_16_16_16', '13?', 'k_32_FLOAT', 'k_32_32_FLOAT']
DIMENSIONS = ['1D', '2D', '3D', 'Cube']
EDRAM_MODES = {0: 'ignore', 4: 'color+depth', 5: 'depth', 6: 'copy'}


def texture(f):
    d0, d1, d2, d3, d4, d5 = (int(x) for x in f)
    dim = (d5 >> 9) & 3
    if dim == 1:
        size = ((d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1, ((d2 >> 26) & 0x3F) + 1)
    elif dim == 2:
        size = ((d2 & 0x7FF) + 1, ((d2 >> 11) & 0x7FF) + 1, ((d2 >> 22) & 0x3FF) + 1)
    else:
        size = ((d2 & 0xFFFFFF) + 1, 1, 1)
    fmt = d1 & 0x3F
    return dict(format=TEXTURE_FORMATS[fmt] if fmt < len(TEXTURE_FORMATS) else f'format{fmt}', endian=(d1 >> 6) & 3, tiled=d0 >> 31,
                pitch=((d0 >> 22) & 0x1FF) << 5, base=(d1 >> 12) << 12, dim=DIMENSIONS[dim], size=size,
                mips=((d4 >> 2) & 0xF, (d4 >> 6) & 0xF), mip_base=(d5 >> 12) << 12, packed=(d5 >> 11) & 1,
                stacked=(d1 >> 10) & 1, swizzle=(d3 >> 1) & 0xFFF, signed=(d0 >> 2) & 0xFF)


def main():
    cp, calls, snaps = dm.load(sys.argv[1], sys.argv[2])
    pairs = dm.pair(cp, calls)
    reg = lambda rec, r: int(rec[dm.reg_index(r)])
    kind_of = lambda i: (dm.API_DRAWS.get(pairs[i][0]['func'], hex(pairs[i][0]['func']))
                         if pairs[i][0] else 'D3D internal (static)')

    textures = {}
    tex_draws = collections.Counter()
    vertex_endians = collections.Counter()
    rts = collections.Counter()
    modes = collections.Counter()
    copies = collections.Counter()
    for i, rec in enumerate(cp):
        kind = kind_of(i)
        mode = reg(rec, 0x2208) & 7
        modes[(kind, EDRAM_MODES.get(mode, mode))] += 1
        surface, color, depth = reg(rec, 0x2000), reg(rec, 0x2001), reg(rec, 0x2002)
        rts[(surface & 0x3FFF, (surface >> 16) & 3, color & 0xFFF, COLOR_FORMATS[(color >> 16) & 0xF],
             depth & 0xFFF, (depth >> 16) & 1)] += 1
        if mode == 6:
            ctl, base, pitch, info = (reg(rec, r) for r in (0x2318, 0x2319, 0x231A, 0x231B))
            copies[(kind, 'src depth' if (ctl & 7) == 4 else f'src color{ctl & 7}',
                    'clear color' if ctl >> 8 & 1 else '', 'clear depth' if ctl >> 9 & 1 else '',
                    f'cmd{(ctl >> 20) & 3}', COLOR_FORMATS[(info >> 7) & 0x3F] if ((info >> 7) & 0x3F) < 16
                    else (info >> 7) & 0x3F, f'endian{info & 7}', f'{pitch & 0x3FFF}x{(pitch >> 16) & 0x3FFF}')] += 1
        if kind == 'D3D internal (static)':
            continue
        for slot in range(32):
            f = rec[dm.reg_index(0x4800 + 6 * slot):dm.reg_index(0x4800 + 6 * slot) + 6]
            if f[0] & 3 == 2:
                t = texture(f)
                key = (t['base'], t['format'], t['size'], t['tiled'], t['dim'])
                textures.setdefault(key, t)
                tex_draws[key] += 1
            elif f[0] & 3 == 3:
                for k in range(3):
                    if (f[2 * k] & 3) == 3:
                        vertex_endians[int(f[2 * k + 1]) & 3] += 1

    print('draws by D3D call and EDRAM mode:')
    for k, v in sorted(modes.items(), key=lambda kv: -kv[1]):
        print(f'  {v:6}  {k}')
    print('\nresolves/copies (mode copy):')
    for k, v in sorted(copies.items(), key=lambda kv: -kv[1]):
        print(f'  {v:6}  {k}')
    print('\nrender target setups (pitch, msaa, color base, color format, depth base, depth format):')
    for k, v in sorted(rts.items(), key=lambda kv: -kv[1])[:20]:
        print(f'  {v:6}  {k}')
    print(f'\ntextures in fetch constants: {len(textures)} distinct')
    by_format = collections.Counter((t['format'], t['dim'], 'tiled' if t['tiled'] else 'linear', t['endian'])
                                    for t in textures.values())
    for k, v in sorted(by_format.items(), key=lambda kv: -kv[1]):
        print(f'  {v:5}  {k}')
    sizes = collections.Counter(t['size'][:2] for t in textures.values())
    print('  most common sizes:', sizes.most_common(8))
    print('  with mip chains:', sum(1 for t in textures.values() if t['mips'][1] > 0),
          '; packed mips:', sum(1 for t in textures.values() if t['packed']),
          '; stacked/3D/cube:', sum(1 for t in textures.values() if t['dim'] != '2D' or t['stacked']))
    print('\nvertex stream endianness (0 none, 1 8in16, 2 8in32, 3 16in32):', dict(vertex_endians))


if __name__ == '__main__':
    main()
