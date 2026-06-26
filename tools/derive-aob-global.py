#!/usr/bin/env python3
# 전역 심볼을 참조하는 ADRP(+ADD|LDR) 사이트를 찾아 유일 시그니처를 만든다.
#  방법: __text 전체를 4바이트씩 보며 ADRP 디코드 → 다음 명령이 ADD/LDR 이면
#        형성 주소가 target(전역) 인지 확인. 후보 사이트마다 ADRP부터 N명령을
#        윈도우로 잡고(ADRP 4B 는 ?? 마스킹) 유일해질 때까지 확장.
# 사용: python3 tools/derive-aob-global.py --macho <fixture> _GUObjectArray
import argparse

from aob_macho import (
    derive_global_adrp_record,
    read_file,
    write_json,
)

def main():
    ap = argparse.ArgumentParser(description="arm64 전역 참조 ADRP AOB 도출")
    ap.add_argument("symbol", help="global Mach-O symbol name")
    ap.add_argument("--macho", required=True, help="입력 Mach-O fixture 경로")
    ap.add_argument("--out", help="JSON 산출물 저장 경로")
    ap.add_argument("--max-instr", type=int, default=15, help="최대 확장 명령 수")
    args = ap.parse_args()

    sym = args.symbol
    data = read_file(args.macho)
    record = derive_global_adrp_record(args.macho, data, sym, args.max_instr)
    if args.out:
        write_json(args.out, record)
    print(
        f"# {sym} via ADRP@{record['reference_pc']} -> {record['target']} "
        f"instrs={record['instruction_count']} matches={record['matches']}"
    )
    print(record["signature"])

if __name__ == "__main__":
    main()
