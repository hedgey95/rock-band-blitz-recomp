"""Checks the XDK D3D device's state against Xenia's registers, draw by draw.

    device_map.py <cp.bin> <dev.bin>

cp.bin comes from the SDK's --trace_draws (patch 6): per draw, the frame, the
draw packet's physical address, the VS/PS IM_LOAD addresses and registers
[0x2000, 0x2400) and [0x4000, 0x4928). dev.bin comes from --trace_device
(src/renderer/device_trace.cpp): every D3D call that wrote commands, with its
arguments and a device snapshot after outermost calls and the game's draw calls.

Model: D3D keeps register shadows in the device (SHADOW) and dirty masks at
device+0..+32. Each draw flushes the dirty part through four helpers (FLUSH);
the GPU sees the shadow *as flushed*, so this replays every flush (its mask
selects registers, its source is the shadow) to build the register file a
native renderer would hold, and compares it with Xenia's at every game draw.
Two kinds of registers are written outside the flush and checked by their own
rule: float constants D3D loads from the shader at bind time (literals), and
fetch slot 31, which Begin(Indexed)Vertices points at inline vertex data.
"""
import collections
import sys

import numpy as np

# (device offset, first register, register count), from the dirty-state flush.
SHADOW = [
    (1152, 0x4800, 192),   # fetch constants: 32 slots x 6
    (1920, 0x4000, 1024),  # VS float constants c0-c255
    (6016, 0x4400, 1024),  # PS float constants c0-c255
    (10112, 0x4900, 8),    # bool constants
    (10272, 0x2388, 24),
    (10368, 0x2000, 16),
    (10444, 0x2100, 21),
    (10528, 0x2180, 5),
    (10548, 0x2200, 12),
    (10596, 0x2280, 21),
    (10680, 0x2300, 38),
    (10832, 0x2380, 8),
]
# Flush helpers: function -> (registers per mask bit, fixed base or None for r5,
# fixed shadow offset or None for r6 - device). Mask bit 63 is index 0.
FLUSH = {
    0x82521420: (1, None, None),
    0x825217B8: (16, None, None),
    0x82521660: (6, 0x4800, 1152),
    0x825214E0: (4, 0x2388, 10272),
}
API_DRAWS = {
    0x8251B220: 'DrawVertices',
    0x8251B638: 'DrawIndexedVertices',
    0x8251A7D8: 'BeginVertices',
    0x8251ACE0: 'BeginIndexedVertices',
    0x8251A7C8: 'Begin/EndVertices',
    0x8251AC98: 'DrawVerticesUP',
}
# Inline-vertex draws: Begin(Indexed)Vertices writes the draw and fetch slot 31
# (pointing at vertex space it reserves in the command buffer) but leaves
# device+48 alone; EndVertices moves it past the data, so these draws land in
# the EndVertices (or DrawVerticesUP) call's range.
INLINE = {0x8251A7C8, 0x8251AC98}
SLOT31 = set(range(0x48BA, 0x48C0))
TILING = {0x2080, 0x2081, 0x2082}  # PA_SC_WINDOW_OFFSET / _SCISSOR_TL / _BR: per tile
PACKET = {0x21F9, 0x21FA, 0x21FB, 0x21FC}  # event initiator, DMA base/size, draw initiator
CPN = 5 + 0x400 + 0x928
HEAD = 15
DEV_BYTES = 16384
IB_BYTES = 32


def reg_index(r):
    return 5 + (r - 0x2000) if r < 0x2400 else 5 + 0x400 + (r - 0x4000)


SHADOW_COL = np.array([reg_index(first + k) - 5 for _, first, n in SHADOW for k in range(n)])
SHADOW_OFF = np.array([off // 4 + k for off, _, n in SHADOW for k in range(n)])
REG_OF_COL = {reg_index(first + k) - 5: first + k for _, first, n in SHADOW for k in range(n)}


def load(cp_path, dev_path):
    cp = np.fromfile(cp_path, dtype='<u4')
    cp = cp[:len(cp) // CPN * CPN].reshape(-1, CPN)
    raw = open(dev_path, 'rb').read()
    calls, snaps, pos = [], [], 0
    while pos + HEAD * 4 <= len(raw):
        h = np.frombuffer(raw, '<u4', HEAD, pos)
        assert h[0] == 0x57524443, f'bad record at {pos}'
        pos += HEAD * 4
        snap = ib = None
        if h[3] >> 31:
            if pos + DEV_BYTES + IB_BYTES > len(raw):
                break
            snap = len(snaps)
            snaps.append(np.frombuffer(raw, '>u4', DEV_BYTES // 4, pos))
            ib = np.frombuffer(raw, '>u4', IB_BYTES // 4, pos + DEV_BYTES)
            pos += DEV_BYTES + IB_BYTES
        a, b = int(h[13]) & 0x1FFFFFFF, int(h[14]) & 0x1FFFFFFF
        calls.append(dict(idx=len(calls), frame=int(h[1]), func=int(h[2]), depth=int(h[3]) & 0x7FFFFFFF,
                          lo=a if a <= b else b - 0x10000, hi=b, snap=snap, ib=ib,
                          device=int(h[4]), mask=(int(h[5]) << 32) | int(h[6]),
                          r4=int(h[6]), r5=int(h[7]), r6=int(h[8]), r7=int(h[9]), r8=int(h[10])))
    return cp, calls, np.array(snaps)


def physical(addr):
    """Xbox 360 virtual -> physical, as the D3D draw code computes it."""
    return (addr & 0x1FFFFFFF) + ((((addr >> 20) & 0xFFF) + 0x200) & 0x1000)


def expected_packet(kind, call, ib):
    """Draw packet fields predicted from the D3D call: prim, index count, index offset, DMA base/size."""
    if kind == 'DrawVertices':  # (dev, prim, start, count)
        return dict(prim=call['r4'], count=call['r6'])
    if kind == 'DrawIndexedVertices':  # (dev, prim, base_vertex, start_index, count); +12812 = IB
        flags, addr = int(ib[0]), int(ib[6])
        size = 4 if flags >> 31 else 2
        return dict(prim=call['r4'], count=call['r7'], offset=call['r5'],
                    dma_base=physical(addr + call['r6'] * size), dma_words=call['r7'])
    if kind == 'DrawVerticesUP':  # (dev, prim, count, data, stride)
        return dict(prim=call['r4'], count=call['r5'])
    if call['func'] == 0x8251A7D8:  # BeginVertices(dev, prim, count, stride)
        return dict(prim=call['r4'], count=call['r5'])
    if call['func'] == 0x8251ACE0:  # BeginIndexedVertices(dev, prim, base_vertex, verts, count, ...)
        return dict(prim=call['r4'], count=call['r7'], offset=call['r5'])
    return None


def flushed_state(calls, snaps):
    """Register file after each snapshot-bearing call, replaying the flushes."""
    width = 0x400 + 0x928
    state = np.zeros(width, dtype=np.uint32)
    state[SHADOW_COL] = snaps[0][SHADOW_OFF]
    pending, out = [], {}
    for c in calls:
        if c['func'] in FLUSH:
            pending.append(c)
        if c['snap'] is None:
            continue
        shadow = snaps[c['snap']]
        for f in pending:
            per_bit, base, off = FLUSH[f['func']]
            base = f['r5'] if base is None else base
            off = (f['r6'] - f['device']) if off is None else off
            for i in range(64):
                if f['mask'] >> (63 - i) & 1:
                    for k in range(per_bit):
                        r = base + i * per_bit + k
                        state[reg_index(r) - 5] = shadow[off // 4 + i * per_bit + k]
        pending = []
        out[c['snap']] = state.copy()
    return out


def pair(cp, calls):
    """Per draw: (outermost call, innermost snapshot-bearing call)."""
    by_frame = collections.defaultdict(list)
    for c in calls:
        by_frame[c['frame']].append(c)
    out = []
    for rec in cp:
        f, p = int(rec[1]), int(rec[2])
        hits = [c for c in by_frame[f] if c['lo'] < p <= c['hi']]
        outer = [c for c in hits if c['depth'] == 0]
        inner = max((c for c in hits if c['snap'] is not None), key=lambda c: c['depth'], default=None)
        out.append((outer[0] if len(outer) == 1 else None, inner))
    return out


def main():
    cp, calls, snaps = load(sys.argv[1], sys.argv[2])
    pairs = pair(cp, calls)
    reg = lambda rows, r: rows[:, reg_index(r)]
    print(f'{len(cp)} draws in frames {sorted(set(cp[:, 1].tolist()))}')

    api = [i for i, (o, n) in enumerate(pairs) if o and o['func'] in API_DRAWS and n]
    other = collections.Counter(hex(o['func']) for o, _ in pairs if o and o['func'] not in API_DRAWS)
    unpaired = [i for i, (o, _) in enumerate(pairs) if o is None]
    print(f'  game draws (D3D draw calls): {len(api)}',
          dict(collections.Counter(API_DRAWS[pairs[i][0]['func']] for i in api)))
    print(f'  other D3D operations that draw (clear/resolve helpers): {sum(other.values())}', dict(other))
    u = cp[unpaired]
    silent = (reg(u, 0x2104) == 0) & ((reg(u, 0x2200) & 0x7) == 0)  # no color writes, no Z/stencil
    print(f'  not written by a traced call: {len(unpaired)} '
          f'(packets at {sorted(set(hex(x) for x in u[:, 2].tolist()))[:2]}..., '
          f'no color/depth/stencil writes in {int(silent.sum())})')

    regs = cp[api]
    xenia = regs[:, 5:][:, SHADOW_COL]
    shadow_at_draw = snaps[[pairs[i][1]['snap'] for i in api]][:, SHADOW_OFF]
    model = flushed_state(calls, snaps)
    flushed = np.array([model[pairs[i][1]['snap']] for i in api])[:, SHADOW_COL]
    vs, ps = regs[:, 3], regs[:, 4]
    inline = np.array([pairs[i][0]['func'] in INLINE for i in api])

    n = xenia.size
    print(f'\ngame draws: {len(api)}; shadow registers: {len(SHADOW_COL)}; values compared: {n}')
    print(f'  device shadow at the draw == Xenia: {int((shadow_at_draw == xenia).sum())}/{n}')
    print(f'  shadow as flushed == Xenia:         {int((flushed == xenia).sum())}/{n}')

    explained = collections.Counter()
    unexplained = collections.defaultdict(list)
    # Last shader change before each draw, per stage.
    bind = {}
    for stage, col in (('vs', vs), ('ps', ps)):
        last, b = 0, []
        for k in range(len(api)):
            if k == 0 or col[k] != col[k - 1] or regs[k, 1] != regs[k - 1, 1]:
                last = k
            b.append(last)
        bind[stage] = b
    for k, j in zip(*np.nonzero(flushed != xenia)):
        r = REG_OF_COL[SHADOW_COL[j]]
        if r in SLOT31 and inline[k]:
            explained['fetch slot 31 = inline vertex data (Begin*Vertices)'] += 1
        elif 0x4000 <= r < 0x4800 and xenia[bind['vs' if r < 0x4400 else 'ps'][k], j] == xenia[k, j]:
            explained['float constant set at the last shader bind (literal)'] += 1
        else:
            unexplained[r].append(k)
    for why, count in explained.items():
        print(f'  explained: {count} values - {why}')
    total_bad = sum(len(v) for v in unexplained.values())
    print(f'  unexplained: {total_bad} values in {len(unexplained)} registers')
    for r, ks in sorted(unexplained.items()):
        print(f'    {r:#06x}: {len(ks)} draws, calls '
              f'{dict(collections.Counter(API_DRAWS[pairs[api[k]][0]["func"]] for k in ks))}')

    # Draw packet fields against the D3D call's arguments.
    packet_bad = collections.Counter()
    packet_checked = collections.Counter()
    for k, i in enumerate(api):
        outer = pairs[i][0]
        kind = API_DRAWS[outer['func']]
        call = outer
        if kind == 'Begin/EndVertices':  # the draw came from the Begin call before this End
            call = next((c for c in reversed(calls[:outer['idx']])
                         if c['func'] in (0x8251A7D8, 0x8251ACE0)), None)
        want = expected_packet(kind, call, outer['ib']) if call else None
        if want is None:
            packet_bad[(kind, 'no call')] += 1
            continue
        di = int(regs[k, reg_index(0x21FC)])
        got = dict(prim=di & 0x3F, count=di >> 16, offset=int(regs[k, reg_index(0x2102)]),
                   dma_base=int(regs[k, reg_index(0x21FA)]),
                   dma_words=int(regs[k, reg_index(0x21FB)]) & 0xFFFFFF)
        for field, value in want.items():
            packet_checked[field] += 1
            if got[field] != value:
                packet_bad[(kind, field)] += 1
    print(f'  draw packet vs call arguments: checked {dict(packet_checked)}; mismatches {dict(packet_bad)}')

    shadowed = set(REG_OF_COL.values())
    print('  varying registers outside the shadow:')
    unmapped = []
    for r in list(range(0x2000, 0x2400)) + list(range(0x4000, 0x4928)):
        want = reg(regs, r)
        if r in shadowed or (want == want[0]).all():
            continue
        why = 'tiling (per tile)' if r in TILING else 'draw packet (call arguments)' if r in PACKET else 'UNMAPPED'
        if why == 'UNMAPPED':
            unmapped.append(r)
        print(f'    {r:#06x}: {len(set(want.tolist()))} values - {why}')

    ok = not unexplained and not unmapped and not packet_bad and len(api) > 0
    print(f'\nN2 gate (every register of every game draw is explained by the flushed shadow, '
          f'shader literals, inline vertices, tiling or the draw call): {"PASS" if ok else "FAIL"}')


if __name__ == '__main__':
    main()
