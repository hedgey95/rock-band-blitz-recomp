"""Find code addresses the game can call that codegen never registered.

Any such address is a future "Call to invalid or unregistered function" crash.
Candidates come from pointers stored in .rdata/.data (vtables, callback tables)
and from addresses built in code with lis + addi/ori (qsort comparators,
thread entry points). A candidate is reported only when it looks like a
function start (the instruction before it ends a block) and no generated
function body covers it.

    python -I tools/audit_code_pointers.py <image.bin> generated/default
"""
import bisect
import glob
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
BLR, BCTR = 0x4E800020, 0x4E800420


def sections(img):
    e = struct.unpack_from("<I", img, 0x3C)[0]
    assert img[e:e + 4] == b"PE\0\0", "not a PE image"
    count = struct.unpack_from("<H", img, e + 6)[0]
    opt = struct.unpack_from("<H", img, e + 20)[0]
    out = {}
    for i in range(count):
        o = e + 24 + opt + 40 * i
        name = img[o:o + 8].rstrip(b"\0").decode()
        vsize, va = struct.unpack_from("<II", img, o + 8)
        out[name] = (IMAGE_BASE + va, IMAGE_BASE + va + vsize)
    return out


def word(img, addr):
    return struct.unpack_from(">I", img, addr - IMAGE_BASE)[0]


def ends_block(insn):
    """True for blr, bctr, an unconditional non-linking b, or zero padding."""
    is_b = (insn >> 26) == 18 and (insn & 1) == 0
    return insn in (BLR, BCTR, 0) or is_b


def function_extents(gen_dir):
    """{start: bytes of instructions emitted} from the generated recompiled bodies."""
    ext, cur = {}, None
    for path in glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp")):
        for line in open(path):
            m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", line)
            if m:
                cur = int(m.group(1), 16)
                ext[cur] = 0
            elif cur is not None and line.startswith("\t// ") and line[4:5].isalpha():
                ext[cur] += 4
    return ext


def code_built_addresses(img, lo, hi):
    """Values formed by lis rD,hi followed (within 8 insns) by addi/ori rX,rD,lo."""
    out = {}
    for a in range(lo, hi - 4, 4):
        insn = word(img, a)
        if insn >> 26 != 15 or (insn >> 16) & 31 != 0:  # lis = addis rD,0,imm
            continue
        rd, upper = (insn >> 21) & 31, (insn & 0xFFFF) << 16
        for b in range(a + 4, min(a + 36, hi), 4):
            nxt = word(img, b)
            op, rs, ra, imm = nxt >> 26, (nxt >> 21) & 31, (nxt >> 16) & 31, nxt & 0xFFFF
            if op == 14 and ra == rd:  # addi rX,rD,simm
                out.setdefault((upper + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF, a)
                break
            if op == 24 and rs == rd:  # ori rX,rD,uimm
                out.setdefault(upper | imm, a)
                break
    return out


def main(image_path, gen_dir):
    img = open(image_path, "rb").read()
    secs = sections(img)
    text_lo, text_hi = secs[".text"]
    ext = function_extents(gen_dir)
    starts = sorted(ext)
    registered = set(ext)  # plus imports and helpers, which have no sub_ body
    for path in glob.glob(os.path.join(gen_dir, "*_init.cpp")):
        registered |= {int(a, 16) for a in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), ", open(path).read())}
    assert starts and all(text_lo <= a < text_hi for a in starts), "generated code does not match image"

    refs = {}  # address -> where it was found
    for name in (".rdata", ".data"):
        lo, hi = secs[name]
        for p in range(lo, min(hi, IMAGE_BASE + len(img)) - 3, 4):
            refs.setdefault(word(img, p), f"data 0x{p:08X}")
    for v, at in code_built_addresses(img, text_lo, text_hi).items():
        refs.setdefault(v, f"lis at 0x{at:08X}")

    gaps = []
    for v, where in refs.items():
        if not (text_lo <= v < text_hi) or v & 3 or v in registered:
            continue
        if word(img, v) == 0 or not ends_block(word(img, v - 4)):
            continue
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and v < starts[i] + ext[starts[i]]:
            continue  # inside a generated function body (jump-table label, handler block)
        gaps.append((v, where, starts[i] if i >= 0 else 0))
    for v, where, prev in sorted(gaps):
        print(f"0x{v:08X} = {{}}  # {where}; after sub_{prev:08X}")
    print(f"# {len(gaps)} uncovered call targets", file=sys.stderr)


if __name__ == "__main__":
    main(*sys.argv[1:3])
