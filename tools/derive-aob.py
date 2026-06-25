#!/usr/bin/env python3
# arm64 함수 앵커의 "유일 시그니처" 도출.
#  심볼 prologue 를 명령 단위로 확장하며 __text 전체 일치 수가 1이 될 때까지 늘린다.
#  PC-상대 명령(ADRP/ADR/B/BL/CB*/TB*/LDR-literal)은 4바이트 전체를 ?? 로 마스킹.
# 사용: python3 tools/derive-aob.py "__ZNK5FName8ToStringER7FString"
import subprocess, sys, struct

GAME = "/Applications/Palworld.app/Contents/MacOS/Palworld"

def nm_addr(sym):
    out = subprocess.run(["nm", GAME], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise SystemExit(f"심볼 못 찾음: {sym}")

def text_section(data):
    # arm64 슬라이스의 __TEXT,__text [fileoff, vmaddr, size] 반환 (FAT/thin 모두)
    magic = struct.unpack_from("<I", data, 0)[0]
    base = 0
    if magic in (0xcafebabe, 0xbebafeca):  # FAT
        nfat = struct.unpack_from(">I", data, 4)[0]
        for i in range(nfat):
            o = 8 + i*20
            cput, off = struct.unpack_from(">I", data, o)[0], struct.unpack_from(">I", data, o+8)[0]
            if cput == 0x0100000c:  # ARM64
                base = off; break
    ncmds = struct.unpack_from("<I", data, base+16)[0]
    p = base + 32
    for _ in range(ncmds):
        cmd, csize = struct.unpack_from("<II", data, p)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[p+8:p+24].split(b"\0")[0]
            if segname == b"__TEXT":
                nsects = struct.unpack_from("<I", data, p+64)[0]
                sp = p + 72
                for _s in range(nsects):
                    sn = data[sp:sp+16].split(b"\0")[0]
                    if sn == b"__text":
                        addr, size = struct.unpack_from("<QQ", data, sp+32)
                        offset = struct.unpack_from("<I", data, sp+48)[0]
                        return base, offset, addr, size
                    sp += 80
        p += csize
    raise SystemExit("__text 없음")

def is_pcrel(word):
    if (word & 0x9f000000) == 0x90000000: return True   # ADRP
    if (word & 0x9f000000) == 0x10000000: return True   # ADR
    if (word & 0x7c000000) == 0x14000000: return True   # B / BL
    if (word & 0x7e000000) == 0x34000000: return True   # CBZ/CBNZ
    if (word & 0x7e000000) == 0x36000000: return True   # TBZ/TBNZ
    if (word & 0xff000000) == 0x54000000: return True   # B.cond
    if (word & 0x3b000000) == 0x18000000: return True   # LDR (literal)
    return False

def masked_count(data, lo, hi, pat, mask):
    n, plen = 0, len(pat)
    i = lo
    while i + plen <= hi:
        ok = True
        for j in range(plen):
            if mask[j] and (data[i+j] & mask[j]) != (pat[j] & mask[j]): ok = False; break
        if ok: n += 1
        i += 1
    return n

def main():
    sym = sys.argv[1]
    data = open(GAME, "rb").read()
    base, toff, taddr, tsize = text_section(data)
    addr = nm_addr(sym)
    foff = base + (addr - taddr) + toff       # 심볼 파일 오프셋
    tlo, thi = base + toff, base + toff + tsize
    pat, mask = bytearray(), bytearray()
    for k in range(1, 16):                    # 최대 15명령까지 확장
        word = struct.unpack_from("<I", data, foff + (k-1)*4)[0]
        b = data[foff + (k-1)*4 : foff + k*4]
        if is_pcrel(word):
            pat += bytes(4); mask += bytes(4)         # 와일드카드 4B
        else:
            pat += b; mask += b"\xff\xff\xff\xff"
        c = masked_count(data, tlo, thi, pat, mask)
        if c == 1:
            sig = " ".join("??" if mask[i]==0 else f"{pat[i]:02X}" for i in range(len(pat)))
            print(f"# {sym}\n# addr={addr:#x} instrs={k} matches=1")
            print(sig)
            return
    raise SystemExit(f"15명령으로도 유일 안 됨: {sym}")

if __name__ == "__main__":
    main()
