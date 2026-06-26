#!/usr/bin/env python3
# arm64 함수 앵커의 "유일 시그니처" 도출.
#  심볼 prologue 를 명령 단위로 확장하며 __text 전체 일치 수가 1이 될 때까지 늘린다.
#  PC-상대 명령(ADRP/ADR/B/BL/CB*/TB*/LDR-literal)은 4바이트 전체를 ?? 로 마스킹.
# 사용: python3 tools/derive-aob.py --macho <fixture> "__ZNK5FName8ToStringER7FString"
import argparse
from aob_macho import (
    derive_function_record,
    read_file,
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
    record = derive_function_record(args.macho, data, sym, args.max_instr)
    if args.out:
        write_json(args.out, record)
    print(
        f"# {sym}\n# macho={args.macho}\n# addr={record['address']} "
        f"instrs={record['instruction_count']} matches={record['matches']}"
    )
    print(record["signature"])

if __name__ == "__main__":
    main()
