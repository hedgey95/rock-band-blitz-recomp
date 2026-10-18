"""Translates captured XDK shader containers to HLSL with XenosRecomp, then DXIL with DXC.

    translate_shaders.py <containers dir> <output dir> [--bound-only] [--xenia <dump dir> [--image <image.bin>]]

Containers come from --capture_shaders (src/renderer/shader_capture.cpp) and are
game data: keep both directories outside the repo. For each <hash>.<vs|ps>.xsc
this writes <hash>.<vs|ps>.hlsl (XenosRecomp, built by tools/xenos_recomp) and
<hash>.<vs|ps>.dxil (dxc.exe from the Windows SDK, with XenosRecomp's DXIL
arguments), and reports every shader that fails either step.

If the containers directory has bound.txt (shaders bound by draws), each must
have a container; --bound-only translates just those (the game registers its
whole shader library at load, most of which a given run never draws with). --xenia compares against the microcode Xenia executed in the
same run (--dump_shaders with --store_shaders=false): every executed program
must be a captured container's microcode, exactly or with only vertex fetches
differing (D3D patches the stream layout into those when it binds a vertex
shader to a vertex declaration; the native renderer takes the layout from the
declaration instead), or one of D3D's built-in shaders, found verbatim in the
executable (--image, from --dump_image).
"""
import concurrent.futures
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XENOS_RECOMP = os.path.join(ROOT, 'out', 'build', 'xenos_recomp', 'XenosRecomp', 'XenosRecomp.exe')
SHADER_COMMON = os.path.join(ROOT, 'thirdparty', 'XenosRecomp', 'XenosRecomp', 'shader_common.h')


def find_dxc():
    kits = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Windows Kits', '10', 'bin')
    found = sorted(glob.glob(os.path.join(kits, '10.*', 'x64', 'dxc.exe')))
    if not found:
        sys.exit('dxc.exe not found in the Windows SDK')
    return found[-1]


def translate(path, out_dir, dxc):
    name = os.path.basename(path)[:-len('.xsc')]
    hlsl = os.path.join(out_dir, name + '.hlsl')
    dxil = os.path.join(out_dir, name + '.dxil')
    try:
        r = subprocess.run([XENOS_RECOMP, path, hlsl, SHADER_COMMON], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return name, 'translate', 'XenosRecomp timed out after 60 s', None
    if r.returncode != 0 or not os.path.exists(hlsl) or os.path.getsize(hlsl) == 0:
        return name, 'translate', (r.stderr or r.stdout or f'exit {r.returncode}').strip(), None
    compile = lambda target: subprocess.run(
        [dxc, '-T', target, '-HV', '2021', '-all-resources-bound', '-Wno-ignored-attributes',
         '-Qstrip_reflect', '-Qstrip_debug', '-Fo', dxil, hlsl], capture_output=True, text=True, timeout=300)
    target = 'ps_6_0' if name.endswith('.ps') else 'vs_6_0'
    r = compile(target)
    if r.returncode != 0 and 'g_SpecConstants' in r.stderr:
        # Specialization constants are resolved at pipeline creation: compile as a
        # library, as XenosRecomp does when its specConstantsMask is set.
        target = 'lib_6_3'
        r = compile(target)
    if r.returncode != 0 or not os.path.exists(dxil):
        return name, 'compile', (r.stderr or r.stdout).strip(), target
    return name, None, '', target


def microcode(path):
    """The container's microcode as little-endian dwords (Xenia dumps it that way)."""
    data = open(path, 'rb').read()
    virtual_size = struct.unpack_from('>I', data, 4)[0]
    shader = struct.unpack_from('>I', data, 0x18)[0]
    offset, size = struct.unpack_from('>II', data, shader)
    code = data[virtual_size + offset:virtual_size + offset + size]
    return tuple(struct.unpack(f'>{size // 4}I', code))


def vertex_fetch_slots(code):
    """Indices of the 3-dword instruction slots holding vertex fetches.

    Control flow comes first, two 48-bit instructions per 3 dwords. Each exec
    instruction covers `count` slots from `address`, with 2 sequence bits per
    slot (bit 0 set: fetch). A fetch whose opcode (bits 0-4) is 0 is a vertex fetch.
    """
    exec_opcodes = {1, 2, 3, 4, 5, 6, 13, 14}
    slots, first, pair = set(), len(code) // 3, 0
    while pair < first and 3 * pair + 2 < len(code):
        d0, d1, d2 = code[3 * pair:3 * pair + 3]
        for cf in (d0 | (d1 & 0xFFFF) << 32, d1 >> 16 | d2 << 16):
            if (cf >> 44) & 0xF in exec_opcodes:
                address, count, sequence = cf & 0xFFF, (cf >> 12) & 0x7, (cf >> 16) & 0xFFF
                first = min(first, address) if address else first
                for i in range(count):
                    slot = address + i
                    if (sequence >> (2 * i)) & 1 and 3 * slot < len(code) and code[3 * slot] & 0x1F == 0:
                        slots.add(slot)
        pair += 1
    return slots


def cross_check(files, dump_dir, image):
    """Every program Xenia executed is a captured container's microcode (exactly, or with
    only the vertex fetches D3D patches at bind differing), or D3D's own (in the executable)."""
    captured = {os.path.basename(p)[:-len('.xsc')]: microcode(p) for p in files}
    image_bytes = open(image, 'rb').read() if image else b''
    for kind, ext in (('vs', 'vert'), ('ps', 'frag')):
        mine = [c for n, c in captured.items() if n.endswith('.' + kind)]
        mine_set = set(mine)
        by_size = {}
        for c in mine:
            by_size.setdefault(len(c), []).append((c, vertex_fetch_slots(c)))
        executed = {}
        for p in glob.glob(os.path.join(dump_dir, f'*.ucode.bin.{ext}')):
            data = open(p, 'rb').read()
            executed[os.path.basename(p)] = struct.unpack(f'<{len(data) // 4}I', data)
        tally = {'exact': 0, 'vertex fetch patched': 0, 'D3D built-in (in the executable)': 0}
        unmatched = []
        for name, code in executed.items():
            if code in mine_set:
                tally['exact'] += 1
            elif any(all(i // 3 in slots for i, (a, b) in enumerate(zip(c, code)) if a != b)
                     for c, slots in by_size.get(len(code), [])):
                tally['vertex fetch patched'] += 1
            elif image_bytes and struct.pack(f'>{len(code)}I', *code) in image_bytes:
                tally['D3D built-in (in the executable)'] += 1
            else:
                unmatched.append(name)
        print(f'  {kind}: Xenia executed {len(executed)}: {tally}; unexplained {len(unmatched)} {unmatched[:4]}')
        CROSS_CHECK_FAILURES.extend(unmatched)


CROSS_CHECK_FAILURES = []


def main():
    args = sys.argv[1:]
    bound_only = '--bound-only' in args
    if bound_only:
        args.remove('--bound-only')
    options = {}
    for flag in ('--xenia', '--image'):
        if flag in args:
            i = args.index(flag)
            options[flag] = args[i + 1]
            del args[i:i + 2]
    src, out_dir = args
    os.makedirs(out_dir, exist_ok=True)
    dxc = find_dxc()
    files = sorted(glob.glob(os.path.join(src, '*.xsc')))
    all_files = files
    bound_file = os.path.join(src, 'bound.txt')
    if bound_only:
        bound = {l.strip() for l in open(bound_file) if l.strip()}
        files = [p for p in files if os.path.basename(p)[:-len('.xsc')] in bound]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda p: translate(p, out_dir, dxc), files))
    failed = [r for r in results if r[1]]
    count = lambda pred: {k: sum(1 for r in results if r[0].endswith('.' + k) and pred(r)) for k in ('vs', 'ps')}
    libs = sum(1 for r in results if not r[1] and r[3] == 'lib_6_3')
    print(f'{len(files)} containers {count(lambda r: True)}: {len(files) - len(failed)} translated and compiled '
          f'{count(lambda r: not r[1])} ({libs} as libraries for specialization constants)')
    for name, step, message, _ in failed:
        first = '\n      '.join(message.splitlines()[:4])
        print(f'  FAILED {step}: {name}\n      {first}')
    if os.path.exists(bound_file):
        bound = [l.strip() for l in open(bound_file) if l.strip()]
        names = {os.path.basename(p)[:-len('.xsc')] for p in all_files}
        missing = [b for b in bound if b not in names]
        ok = {r[0] for r in results if not r[1]}
        print(f'  bound by draws: {len(bound)} {dict((k, sum(b.endswith("." + k) for b in bound)) for k in ("vs", "ps"))}; '
              f'without a container: {len(missing)}; translated and compiled: {sum(b in ok for b in bound)}')
        failed += [(b, 'bound without container', '', None) for b in missing]
    if '--xenia' in options:
        cross_check(all_files, options['--xenia'], options.get('--image'))
    sys.exit(1 if failed or CROSS_CHECK_FAILURES else 0)


if __name__ == '__main__':
    main()
