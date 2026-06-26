#!/usr/bin/env python3
# 전역 심볼을 참조하는 ADRP(+ADD|LDR) 사이트를 찾아 유일 시그니처를 만든다.
#  방법: __text 전체를 4바이트씩 보며 ADRP 디코드 → 다음 명령이 ADD/LDR 이면
#        형성 주소가 target(전역) 인지 확인. 후보 사이트마다 ADRP부터 N명령을
#        윈도우로 잡고(ADRP 4B 는 ?? 마스킹) 유일해질 때까지 확장.
# 사용: python3 tools/derive-aob-global.py --macho <fixture> _GUObjectArray
import argparse
import struct

from aob_macho import (
    find_symbol,
    fixture_metadata,
    format_sig,
    is_pcrel,
    masked_count,
    read_file,
    text_section,
    write_json,
)

def adrp_decode(word, pc):
    immlo = (word >> 29) & 3; immhi = (word >> 5) & 0x7ffff
    imm = (immhi << 2) | immlo
    if imm & (1 << 20): imm -= (1 << 21)
    return (pc & ~0xfff) + (imm << 12)

def main():
    ap = argparse.ArgumentParser(description="arm64 전역 참조 ADRP AOB 도출")
    ap.add_argument("symbol", help="global Mach-O symbol name")
    ap.add_argument("--macho", required=True, help="입력 Mach-O fixture 경로")
    ap.add_argument("--out", help="JSON 산출물 저장 경로")
    ap.add_argument("--max-instr", type=int, default=15, help="최대 확장 명령 수")
    args = ap.parse_args()

    sym = args.symbol
    data = read_file(args.macho)
    text = text_section(data)
    target = find_symbol(data, sym)
    tlo = text.slice_base + text.fileoff
    thi = tlo + text.size
    for off in range(tlo, thi - 8, 4):
        w0 = struct.unpack_from("<I", data, off)[0]
        if (w0 & 0x9f000000) != 0x90000000: continue      # ADRP?
        pc = text.vmaddr + (off - tlo)
        page = adrp_decode(w0, pc)
        w1 = struct.unpack_from("<I", data, off+4)[0]
        if (w1 & 0xff800000) == 0x91000000:                # ADD imm
            formed = page + ((w1 >> 10) & 0xfff)
        elif (w1 & 0xffc00000) == 0xf9400000:              # LDR imm
            formed = page + (((w1 >> 10) & 0xfff) << 3)
        else:
            continue
        if formed != target: continue
        # 이 사이트에서 ADRP(??) 시작으로 윈도우 확장 → 유일 시그니처
        pat, mask = bytearray(b"\0\0\0\0"), bytearray(b"\0\0\0\0")   # ADRP 마스킹
        for k in range(2, args.max_instr + 1):
            word = struct.unpack_from("<I", data, off + (k-1)*4)[0]
            b = data[off + (k-1)*4 : off + k*4]
            if is_pcrel(word): pat += bytes(4); mask += bytes(4)
            else: pat += b; mask += b"\xff\xff\xff\xff"
            if masked_count(data, tlo, thi, pat, mask) == 1:
                sig = format_sig(pat, mask)
                record = {
                    "kind": "global_adrp",
                    "symbol": sym,
                    "target": f"0x{target:x}",
                    "reference_pc": f"0x{pc:x}",
                    "instruction_count": k,
                    "matches": 1,
                    "signature": sig,
                    "fixture": fixture_metadata(args.macho, data, text),
                }
                if args.out:
                    write_json(args.out, record)
                print(f"# {sym} via ADRP@{pc:#x} -> {target:#x} instrs={k} matches=1")
                print(sig)
                return
    raise SystemExit(f"유일 ADRP 사이트 못 만듦: {sym}  (폴백: 심볼 유지)")

if __name__ == "__main__":
    main()
