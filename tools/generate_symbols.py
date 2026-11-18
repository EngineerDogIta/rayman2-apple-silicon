#!/usr/bin/env python3
"""Generate SwARc symbol tables (functions.tsv, jumptables.tsv) for PE32 binaries.

Combines:
1. PE entry point, exports.tsv, .data/.rdata/.reloc function pointers.
2. Known ACP_Ray2 / Ray2Fix named function VAs (when matching Rayman2.exe).
3. Disassembly via i686-w64-mingw32-objdump for direct CALL targets and
   MSVC6 switch jump tables (both .text-embedded and .rdata tables).

Usage:
  python3 tools/generate_symbols.py \
      --pe /path/to/bin --symdir symbols/mod --name rayman2 \
      [--acp-src third_party/ACP_Ray2/src]
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class PEImage:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.d = f.read()
        e_lfanew = u32(self.d, 0x3C)
        assert self.d[e_lfanew : e_lfanew + 4] == b"PE\0\0", f"{path}: not PE"
        fh = e_lfanew + 4
        self.nsec = u16(self.d, fh + 2)
        opt_size = u16(self.d, fh + 16)
        oh = fh + 20
        assert u16(self.d, oh) == 0x10B, f"{path}: expected PE32"
        self.entry_rva = u32(self.d, oh + 16)
        self.image_base = u32(self.d, oh + 28)
        self.image_size = u32(self.d, oh + 56)
        self.headers_size = u32(self.d, oh + 60)
        ndirs = u32(self.d, oh + 92)
        self.dirs = [
            (u32(self.d, oh + 96 + i * 8), u32(self.d, oh + 100 + i * 8))
            for i in range(ndirs)
        ]
        self.sections = []
        sh = oh + opt_size
        for i in range(self.nsec):
            o = sh + i * 40
            name = self.d[o : o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", self.d, o + 8)
            chars = u32(self.d, o + 36)
            self.sections.append((name, va, vsize, rptr, rsize, chars))

        # Locate .text bounds and readable initialized data ranges
        self.text_ranges = []
        self.data_ranges = []
        for name, va, vsize, rptr, rsize, chars in self.sections:
            vlo = self.image_base + va
            # Align extent to virtual size vs raw size
            ext = max(vsize, rsize)
            vhi = vlo + ext
            is_exec = bool(chars & 0x20000000) or name == ".text"
            is_init_data = bool(chars & 0x00000040)
            if is_exec:
                self.text_ranges.append((vlo, self.image_base + va + vsize, rptr, min(vsize, rsize)))
            elif is_init_data and name not in (".rsrc", ".reloc"):
                # Read up to raw size initialized bytes
                self.data_ranges.append((vlo, vlo + min(vsize, rsize) if vsize else vlo + rsize, rptr))

    def in_text(self, va: int) -> bool:
        for lo, hi, _, _ in self.text_ranges:
            if lo <= va < hi:
                return True
        return False

    def va_to_off(self, va: int):
        rva = va - self.image_base
        if rva < 0:
            return None
        for _, sva, vsize, rptr, rsize, _ in self.sections:
            if sva <= rva < sva + rsize:
                return rptr + (rva - sva)
        return None

    def read_u32(self, va: int):
        off = self.va_to_off(va)
        if off is None or off + 4 > len(self.d):
            return None
        return u32(self.d, off)

    def read_bytes(self, va: int, n: int):
        off = self.va_to_off(va)
        if off is None or off + n > len(self.d):
            return b""
        return self.d[off : off + n]

    def relocs(self):
        if len(self.dirs) <= 5 or self.dirs[5][1] == 0:
            return set()
        rva, size = self.dirs[5]
        off = self.va_to_off(self.image_base + rva)
        if off is None:
            return set()
        end = off + size
        targets = set()
        while off + 8 <= end and off + 8 <= len(self.d):
            page_rva, blk_size = struct.unpack_from("<II", self.d, off)
            if blk_size < 8:
                break
            n_entries = (blk_size - 8) // 2
            for i in range(n_entries):
                entry = u16(self.d, off + 8 + i * 2)
                rtype = entry >> 12
                roffs = entry & 0x0FFF
                if rtype == 3:  # IMAGE_REL_BASED_HIGHLOW
                    fix_va = self.image_base + page_rva + roffs
                    val = self.read_u32(fix_va)
                    if val is not None:
                        targets.add((fix_va, val))
            off += blk_size
        return targets


def parse_acp_names(acp_src: str):
    """Extract ACP_Ray2 OFFSET(0x...) function names -> va map."""
    names = {}
    if not acp_src or not os.path.isdir(acp_src):
        return names
    # Match lines like: void (*GAM_fn_vEngine)( void ) = OFFSET(0x40ADA0);
    # Avoid matching *const variables which are data globals.
    rx = re.compile(
        r"\(\s*\*+\s*([A-Za-z0-9_]+)\s*\)\s*\([^;]*\)\s*=\s*OFFSET\(\s*(0x[0-9A-Fa-f]+)\s*\)"
    )
    for cfile in glob.glob(os.path.join(acp_src, "**", "*.c"), recursive=True):
        with open(cfile, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
        # Normalize multi-line declarations
        flat = " ".join(text.splitlines())
        # Process statement by statement
        for stmt in flat.split(";"):
            m = rx.search(stmt)
            if m:
                sym = m.group(1)
                va = int(m.group(2), 16)
                names[va] = sym
    return names


def read_exports_tsv(symdir: str):
    names = {}
    exp_path = os.path.join(symdir, "exports.tsv")
    if not os.path.isfile(exp_path):
        return names
    with open(exp_path, "r", encoding="utf-8") as f:
        next(f, None)
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 3 and cols[1] == "func":
                va = int(cols[0], 16)
                # Prefer simple C identifier or mangled name
                sym = cols[2]
                names[va] = sym
    return names


def read_imports_set(symdir: str):
    iat = set()
    imp_path = os.path.join(symdir, "imports.tsv")
    if not os.path.isfile(imp_path):
        return iat
    with open(imp_path, "r", encoding="utf-8") as f:
        next(f, None)
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if cols:
                iat.add(int(cols[0], 16))
    return iat


def run_objdump(pe_path: str):
    tool = shutil.which("i686-w64-mingw32-objdump")
    if not tool:
        raise RuntimeError("i686-w64-mingw32-objdump not found in PATH")
    res = subprocess.run(
        [tool, "-d", pe_path],
        capture_output=True,
        text=True,
        check=True,
    )
    return res.stdout


def extract_disasm_info(pe: PEImage, disasm_text: str, iat_set: set):
    """Scan objdump output for direct CALL targets and switch jump tables."""
    call_rx = re.compile(r"^\s*([0-9a-fA-F]+):.*\bcall\s+0x([0-9a-fA-F]+)\b")
    jmp_ind_rx = re.compile(
        r"^\s*([0-9a-fA-F]+):.*\bjmp[l]?\s+\*0x([0-9a-fA-F]+)\(,%([a-z0-9]+),4\)"
    )
    cmp_imm_rx = re.compile(r"\bcmp\s+\$0x([0-9a-fA-F]+),\s*%([a-z0-9]+)\b")
    # MSVC6 sparse switch indirection: mov 0x...( %eax ), %cl  (or %al/%dl/%bl)
    idx_byte_rx = re.compile(r"\bmov(?:zbl)?\s+0x([0-9a-fA-F]+)\(%[a-z0-9]+\),\s*%([abcd]l)\b")
    decoded_insns = []  # list of (va, line)
    call_targets = set()

    for line in disasm_text.splitlines():
        if ":" not in line:
            continue
        m_va = re.match(r"^\s*([0-9a-fA-F]+):\s", line)
        if not m_va:
            continue
        va = int(m_va.group(1), 16)
        decoded_insns.append((va, line))
        mc = call_rx.match(line)
        if mc:
            t = int(mc.group(2), 16)
            if pe.in_text(t):
                call_targets.add(t)

    jumptables = {}
    for idx, (va, line) in enumerate(decoded_insns):
        mj = jmp_ind_rx.match(line)
        if not mj:
            continue
        tbl_va = int(mj.group(2), 16)
        idx_reg = mj.group(3)
        if tbl_va in iat_set:
            continue

        # Look back up to 16 instructions for cmp $max, %reg and sparse byte-index table
        limit = None
        byte_tbl_va = None
        for back in range(max(0, idx - 16), idx):
            _, bline = decoded_insns[back]
            mc = cmp_imm_rx.search(bline)
            if mc:
                cval = int(mc.group(1), 16)
                if cval < 512:
                    limit = cval + 1
            mb = idx_byte_rx.search(bline)
            if mb:
                bva = int(mb.group(1), 16)
                breg8 = mb.group(2)
                if breg8[0] == idx_reg[1] if len(idx_reg) == 3 else False:
                    byte_tbl_va = bva

        if byte_tbl_va is not None and limit is not None and 1 <= limit <= 512:
            idx_bytes = pe.read_bytes(byte_tbl_va, limit)
            if len(idx_bytes) == limit:
                max_entries = max(idx_bytes) + 1
            else:
                max_entries = 256
        elif limit is not None and 1 <= limit <= 512:
            max_entries = limit
        else:
            max_entries = 256

        targets = []
        for k in range(max_entries):
            t = pe.read_u32(tbl_va + k * 4)
            if t is None or not pe.in_text(t):
                break
            # Switch cases always reside inside the same function neighborhood
            if abs(t - va) > 0x20000:
                break
            targets.append(t)

        if len(targets) >= 2 and (byte_tbl_va is not None or limit is None or len(targets) == limit or abs(targets[0] - va) < 0x100):
            jumptables[va] = sorted(set(targets))

    return call_targets, jumptables


def scan_data_and_reloc_ptrs(pe: PEImage):
    ptrs = set()
    for vlo, vhi, rptr in pe.data_ranges:
        n = (vhi - vlo) & ~3
        for off in range(0, n, 4):
            val = u32(pe.d, rptr + off)
            if pe.in_text(val):
                # Reject obvious ASCII/garbage unless aligned or prologue-like,
                # though SwARc also checks decode. We filter non-int3 padding.
                b0 = pe.read_bytes(val, 2)
                if b0 and b0[0] not in (0x00, 0xCC, 0x90):
                    ptrs.add(val)

    for fix_va, val in pe.relocs():
        if pe.in_text(val):
            b0 = pe.read_bytes(val, 2)
            if b0 and b0[0] not in (0x00, 0xCC, 0x90):
                # Also avoid reloc inside the jump table itself? Having case labels
                # as weak candidates won't hurt ONLY IF they aren't strong entries;
                # actually better to exclude reloc inside known .text jump tables.
                ptrs.add(val)
    return ptrs


RAY2_MANUAL_NAMES = {
    0x4016E0: "R2_fn_bAppliOnDeActivate",
    0x401790: "R2_fn_bAppliOnActivate",
    0x4022D0: "GAM_fn_WndProc",
    0x413890: "IPT_fn_vEngineReadInput",
    0x422940: "GLI_vGet3DVertexFromScreenPos",
    0x422AB0: "GLI_xAdjustCameraToViewport2",
    0x429670: "GLI_vDraw2DSpriteWithPercent",
    0x446590: "POS_fn_vSetTranslationVector",
    0x465C10: "JFFTXT_vAffiche",
    0x4710C0: "R2_fn_p_stSPOSuperimpoed",
    0x475680: "AI_fn_p_stEvalHUDProcedure409_410",
    0x4829D0: "R2_fn_szGetStringFromTextOrStringParam",
    0x494CF0: "INO_fn_wInit",
    0x495170: "R2_fn_InputEnum",
    0x496590: "INO_fn_wReadKeyboard",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", required=True)
    ap.add_argument("--symdir", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--acp-src", default="")
    args = ap.parse_args()

    pe = PEImage(args.pe)
    os.makedirs(args.symdir, exist_ok=True)

    names = {}
    if args.acp_src:
        for va, sym in parse_acp_names(args.acp_src).items():
            if pe.in_text(va):
                names[va] = sym
    if args.name == "rayman2":
        for va, sym in RAY2_MANUAL_NAMES.items():
            if pe.in_text(va):
                names[va] = sym

    exp_names = read_exports_tsv(args.symdir)
    for va, sym in exp_names.items():
        if pe.in_text(va):
            names[va] = sym

    iat_set = read_imports_set(args.symdir)
    disasm = run_objdump(args.pe)
    call_targets, jumptables = extract_disasm_info(pe, disasm, iat_set)

    # Do NOT treat jump-table targets inside functions as independent function entries
    jt_all_targets = set()
    for ts in jumptables.values():
        jt_all_targets.update(ts)

    entry_va = pe.image_base + pe.entry_rva
    funcs = set()
    if pe.in_text(entry_va):
        funcs.add(entry_va)
        names.setdefault(entry_va, "entry_point")
    funcs.update(names.keys())
    funcs.update(call_targets)

    # Exclude raw .reloc/.data sweep from strong entries: SwARc's own lifter pass 1
    # discovers immediate code pointers & .data dwords safely as weak entries so
    # switch case labels inside .reloc are never mistakenly split off as functions.
    # Here we add known Rayman2 engine dispatch tables in .data explicitly.
    if args.name == "rayman2":
        for tbl_va, count, stride in ((0x4A4180, 41, 4), (0x4A5E80, 23, 4), (0x4A5EE0, 169, 8)):
            for k in range(count):
                fn = pe.read_u32(tbl_va + k * stride)
                if fn is not None and pe.in_text(fn) and fn not in jt_all_targets:
                    funcs.add(fn)

    # Also detect contiguous MSVC6 prologue signatures right after INT3/NOP/RET padding
    for vlo, vhi, rptr, rsz in pe.text_ranges:
        code = pe.d[rptr : rptr + rsz]
        # Only inspect 16-byte or 4-byte aligned addresses after 0xCC/0xC3/0x90
        for off in range(1, len(code) - 4):
            va = vlo + off
            if (va & 0xF) != 0 and (va & 0x3) != 0:
                continue
            prev = code[off - 1]
            if prev not in (0xCC, 0x90, 0xC3):
                continue
            b = code[off : off + 3]
            if b == b"\x55\x8b\xec" or b[:2] == b"\x81\xec" or b[:2] == b"\x83\xec":
                if va not in jt_all_targets and va not in jumptables:
                    funcs.add(va)

    sorted_funcs = sorted(funcs)
    fn_path = os.path.join(args.symdir, "functions.tsv")
    with open(fn_path, "w", encoding="utf-8") as f:
        f.write("entry\tmin\tmax\tchunks\tthunk\tnoreturn\tcallconv\tname\n")
        for i, va in enumerate(sorted_funcs):
            nxt = sorted_funcs[i + 1] if i + 1 < len(sorted_funcs) else (pe.text_ranges[0][1])
            max_va = max(va, nxt - 1)
            sym = names.get(va, f"FUN_{va:08X}")
            f.write(f"{va:08X}\t{va:08X}\t{max_va:08X}\t1\t0\t0\tunknown\t{sym}\n")

    jt_path = os.path.join(args.symdir, "jumptables.tsv")
    with open(jt_path, "w", encoding="utf-8") as f:
        f.write("jmp\ttargets\n")
        for jmp_va in sorted(jumptables.keys()):
            tlist = ",".join(f"{t:08X}" for t in jumptables[jmp_va])
            f.write(f"{jmp_va:08X}\t{tlist}\n")

    print(
        f"[{args.name}] {len(sorted_funcs)} functions ({len(names)} named, "
        f"{len(call_targets)} direct calls), {len(jumptables)} jump tables -> {args.symdir}"
    )


if __name__ == "__main__":
    main()
