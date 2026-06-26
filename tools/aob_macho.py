#!/usr/bin/env python3
import hashlib
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path


CPU_TYPE_ARM64 = 0x0100000C
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_UUID = 0x1B


@dataclass(frozen=True)
class TextSection:
    slice_base: int
    segment: str
    section: str
    fileoff: int
    vmaddr: int
    size: int


def read_file(path: str) -> bytes:
    return Path(path).read_bytes()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def arm64_slice_base(data: bytes) -> int:
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic == FAT_MAGIC:
        nfat = struct.unpack_from(">I", data, 4)[0]
        for i in range(nfat):
            off = 8 + i * 20
            cputype = struct.unpack_from(">I", data, off)[0]
            slice_off = struct.unpack_from(">I", data, off + 8)[0]
            if cputype == CPU_TYPE_ARM64:
                return slice_off
        raise SystemExit("arm64 슬라이스 없음")
    if magic == FAT_MAGIC_64:
        nfat = struct.unpack_from(">I", data, 4)[0]
        for i in range(nfat):
            off = 8 + i * 32
            cputype = struct.unpack_from(">I", data, off)[0]
            slice_off = struct.unpack_from(">Q", data, off + 8)[0]
            if cputype == CPU_TYPE_ARM64:
                return slice_off
        raise SystemExit("arm64 슬라이스 없음")
    return 0


def require_macho64(data: bytes, base: int) -> None:
    magic = struct.unpack_from("<I", data, base)[0]
    if magic != MH_MAGIC_64:
        raise SystemExit(f"MH_MAGIC_64 불일치: 0x{magic:x}")


def load_commands(data: bytes, base: int):
    require_macho64(data, base)
    ncmds = struct.unpack_from("<I", data, base + 16)[0]
    p = base + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, p)
        yield p, cmd, cmdsize
        p += cmdsize


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", errors="replace")


def text_section(data: bytes) -> TextSection:
    base = arm64_slice_base(data)
    for p, cmd, _cmdsize in load_commands(data, base):
        if cmd != LC_SEGMENT_64:
            continue
        segname = cstr(data[p + 8:p + 24])
        nsects = struct.unpack_from("<I", data, p + 64)[0]
        sp = p + 72
        for _ in range(nsects):
            sectname = cstr(data[sp:sp + 16])
            if sectname == "__text":
                vmaddr, size = struct.unpack_from("<QQ", data, sp + 32)
                fileoff = struct.unpack_from("<I", data, sp + 48)[0]
                return TextSection(base, segname, sectname, fileoff, vmaddr, size)
            sp += 80
    raise SystemExit("__text 없음")


def macho_uuid(data: bytes) -> str | None:
    base = arm64_slice_base(data)
    for p, cmd, _cmdsize in load_commands(data, base):
        if cmd == LC_UUID:
            raw = data[p + 8:p + 24]
            h = raw.hex()
            return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"
    return None


def segment_hashes(data: bytes) -> dict[str, str]:
    base = arm64_slice_base(data)
    hashes: dict[str, str] = {}
    for p, cmd, _cmdsize in load_commands(data, base):
        if cmd != LC_SEGMENT_64:
            continue
        segname = cstr(data[p + 8:p + 24])
        fileoff, filesize = struct.unpack_from("<QQ", data, p + 40)
        if filesize:
            seg = data[base + fileoff:base + fileoff + filesize]
            hashes[segname] = sha256_hex(seg)
    return hashes


def find_symbol(data: bytes, symbol_name: str) -> int:
    base = arm64_slice_base(data)
    for p, cmd, _cmdsize in load_commands(data, base):
        if cmd != LC_SYMTAB:
            continue
        symoff, nsyms, stroff, _strsize = struct.unpack_from("<IIII", data, p + 8)
        syms = base + symoff
        strs = base + stroff
        for i in range(nsyms):
            ent = syms + i * 16
            strx = struct.unpack_from("<I", data, ent)[0]
            if strx == 0:
                continue
            end = data.find(b"\0", strs + strx)
            if end < 0:
                continue
            name = data[strs + strx:end].decode("utf-8", errors="replace")
            if name == symbol_name:
                return struct.unpack_from("<Q", data, ent + 8)[0]
        break
    raise SystemExit(f"심볼 못 찾음: {symbol_name}")


def is_pcrel(word: int) -> bool:
    if (word & 0x9F000000) == 0x90000000:
        return True   # ADRP
    if (word & 0x9F000000) == 0x10000000:
        return True   # ADR
    if (word & 0x7C000000) == 0x14000000:
        return True   # B / BL
    if (word & 0x7E000000) == 0x34000000:
        return True   # CBZ/CBNZ
    if (word & 0x7E000000) == 0x36000000:
        return True   # TBZ/TBNZ
    if (word & 0xFF000000) == 0x54000000:
        return True   # B.cond
    if (word & 0x3B000000) == 0x18000000:
        return True   # LDR literal
    return False


def masked_count(data: bytes, lo: int, hi: int, pat: bytes | bytearray, mask: bytes | bytearray) -> int:
    count = 0
    plen = len(pat)
    if plen == 0 or hi - lo < plen:
        return 0

    anchor = next((i for i, m in enumerate(mask) if m), None)
    if anchor is None:
        return max(0, hi - lo - plen + 1)

    pos = lo + anchor
    end = hi - plen + anchor
    needle = bytes([pat[anchor]])
    while pos <= end:
        hit = data.find(needle, pos, end + 1)
        if hit < 0:
            break
        i = hit - anchor
        ok = True
        for j in range(plen):
            if mask[j] and (data[i + j] & mask[j]) != (pat[j] & mask[j]):
                ok = False
                break
        if ok:
            count += 1
        pos = hit + 1
    return count


def format_sig(pat: bytes | bytearray, mask: bytes | bytearray) -> str:
    return " ".join("??" if mask[i] == 0 else f"{pat[i]:02X}" for i in range(len(pat)))


def fixture_metadata(path: str, data: bytes, text: TextSection) -> dict:
    text_start = text.slice_base + text.fileoff
    text_bytes = data[text_start:text_start + text.size]
    return {
        "path": str(Path(path)),
        "file_size": len(data),
        "file_sha256": sha256_hex(data),
        "macho_uuid": macho_uuid(data),
        "text_section": {
            **asdict(text),
            "vmaddr": f"0x{text.vmaddr:x}",
            "fileoff": f"0x{text.fileoff:x}",
            "size": f"0x{text.size:x}",
            "sha256": sha256_hex(text_bytes),
        },
        "segment_sha256": segment_hashes(data),
    }


def write_json(path: str, payload: dict) -> None:
    Path(path).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def derive_function_record(path: str, data: bytes, symbol: str, max_instr: int = 15) -> dict:
    text = text_section(data)
    addr = find_symbol(data, symbol)
    if not (text.vmaddr <= addr < text.vmaddr + text.size):
        raise SystemExit(f"심볼이 __text 범위 밖에 있음: {symbol} addr=0x{addr:x}")

    foff = text.slice_base + text.fileoff + (addr - text.vmaddr)
    tlo = text.slice_base + text.fileoff
    thi = tlo + text.size
    pat, mask = bytearray(), bytearray()
    for k in range(1, max_instr + 1):
        word = struct.unpack_from("<I", data, foff + (k - 1) * 4)[0]
        b = data[foff + (k - 1) * 4:foff + k * 4]
        if is_pcrel(word):
            pat += bytes(4)
            mask += bytes(4)
        else:
            pat += b
            mask += b"\xff\xff\xff\xff"
        count = masked_count(data, tlo, thi, pat, mask)
        if count == 1:
            return {
                "kind": "function",
                "symbol": symbol,
                "address": f"0x{addr:x}",
                "instruction_count": k,
                "matches": count,
                "signature": format_sig(pat, mask),
                "fixture": fixture_metadata(path, data, text),
            }
    raise SystemExit(f"{max_instr}명령으로도 유일 안 됨: {symbol}")


def adrp_decode(word: int, pc: int) -> int:
    immlo = (word >> 29) & 3
    immhi = (word >> 5) & 0x7ffff
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= (1 << 21)
    return (pc & ~0xfff) + (imm << 12)


def derive_global_adrp_record(path: str, data: bytes, symbol: str, max_instr: int = 15) -> dict:
    text = text_section(data)
    target = find_symbol(data, symbol)
    tlo = text.slice_base + text.fileoff
    thi = tlo + text.size
    for off in range(tlo, thi - 8, 4):
        w0 = struct.unpack_from("<I", data, off)[0]
        if (w0 & 0x9f000000) != 0x90000000:
            continue
        pc = text.vmaddr + (off - tlo)
        page = adrp_decode(w0, pc)
        w1 = struct.unpack_from("<I", data, off + 4)[0]
        if (w1 & 0xff800000) == 0x91000000:
            formed = page + ((w1 >> 10) & 0xfff)
        elif (w1 & 0xffc00000) == 0xf9400000:
            formed = page + (((w1 >> 10) & 0xfff) << 3)
        else:
            continue
        if formed != target:
            continue

        pat, mask = bytearray(b"\0\0\0\0"), bytearray(b"\0\0\0\0")
        for k in range(2, max_instr + 1):
            word = struct.unpack_from("<I", data, off + (k - 1) * 4)[0]
            b = data[off + (k - 1) * 4:off + k * 4]
            if is_pcrel(word):
                pat += bytes(4)
                mask += bytes(4)
            else:
                pat += b
                mask += b"\xff\xff\xff\xff"
            if masked_count(data, tlo, thi, pat, mask) == 1:
                return {
                    "kind": "global_adrp",
                    "symbol": symbol,
                    "target": f"0x{target:x}",
                    "reference_pc": f"0x{pc:x}",
                    "instruction_count": k,
                    "matches": 1,
                    "signature": format_sig(pat, mask),
                    "fixture": fixture_metadata(path, data, text),
                }
    raise SystemExit(f"유일 ADRP 사이트 못 만듦: {symbol}")
