#!/usr/bin/env python3
# arm64 함수 앵커의 "유일 시그니처" 도출.
#  심볼 prologue 를 명령 단위로 확장하며 __text 전체 일치 수가 1이 될 때까지 늘린다.
#  PC-상대 명령(ADRP/ADR/B/BL/CB*/TB*/LDR-literal)은 4바이트 전체를 ?? 로 마스킹.
# 사용: python3 tools/derive-aob.py --macho <fixture> "__ZNK5FName8ToStringER7FString"
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

def main():
    ap = argparse.ArgumentParser(description="arm64 함수 앵커 AOB 도출")
    ap.add_argument("symbol", help="mangled Mach-O symbol name")
    ap.add_argument("--macho", required=True, help="입력 Mach-O fixture 경로")
    ap.add_argument("--out", help="JSON 산출물 저장 경로")
    ap.add_argument("--max-instr", type=int, default=15, help="최대 확장 명령 수")
    args = ap.parse_args()

    sym = args.symbol
    data = read_file(args.macho)
    text = text_section(data)
    addr = find_symbol(data, sym)
    if not (text.vmaddr <= addr < text.vmaddr + text.size):
        raise SystemExit(f"심볼이 __text 범위 밖에 있음: {sym} addr=0x{addr:x}")

    foff = text.slice_base + text.fileoff + (addr - text.vmaddr)
    tlo = text.slice_base + text.fileoff
    thi = tlo + text.size
    pat, mask = bytearray(), bytearray()
    for k in range(1, args.max_instr + 1):
        word = struct.unpack_from("<I", data, foff + (k-1)*4)[0]
        b = data[foff + (k-1)*4 : foff + k*4]
        if is_pcrel(word):
            pat += bytes(4); mask += bytes(4)         # 와일드카드 4B
        else:
            pat += b; mask += b"\xff\xff\xff\xff"
        c = masked_count(data, tlo, thi, pat, mask)
        if c == 1:
            sig = format_sig(pat, mask)
            record = {
                "kind": "function",
                "symbol": sym,
                "address": f"0x{addr:x}",
                "instruction_count": k,
                "matches": c,
                "signature": sig,
                "fixture": fixture_metadata(args.macho, data, text),
            }
            if args.out:
                write_json(args.out, record)
            print(f"# {sym}\n# macho={args.macho}\n# addr={addr:#x} instrs={k} matches=1")
            print(sig)
            return
    raise SystemExit(f"{args.max_instr}명령으로도 유일 안 됨: {sym}")

if __name__ == "__main__":
    main()
