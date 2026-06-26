#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path

from aob_macho import (
    derive_function_record,
    derive_global_adrp_record,
    fixture_metadata,
    read_file,
    text_section,
    write_json,
)


def slugify(name: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_")
    return slug or "anchor"


def derive_anchor(path: str, data: bytes, anchor: dict, max_instr: int) -> dict:
    kind = anchor["kind"]
    symbol = anchor["symbol"]
    if kind == "function":
        record = derive_function_record(path, data, symbol, max_instr)
    elif kind == "global_adrp":
        record = derive_global_adrp_record(path, data, symbol, max_instr)
    else:
        raise SystemExit(f"지원하지 않는 앵커 종류: {kind}")
    return {
        "name": anchor["name"],
        **record,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="앵커 manifest 기반 AOB 일괄 도출")
    ap.add_argument("--macho", required=True, help="입력 Mach-O fixture 경로")
    ap.add_argument(
        "--anchors",
        default=str(Path(__file__).with_name("aob-anchors-ue51.json")),
        help="앵커 정의 JSON 경로",
    )
    ap.add_argument("--out-dir", required=True, help="앵커별 JSON 산출물 디렉토리")
    ap.add_argument("--max-instr", type=int, default=15, help="최대 확장 명령 수")
    args = ap.parse_args()

    data = read_file(args.macho)
    anchors_doc = json.loads(Path(args.anchors).read_text(encoding="utf-8"))
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    records = []
    for anchor in anchors_doc["anchors"]:
        record = derive_anchor(args.macho, data, anchor, args.max_instr)
        records.append(record)
        write_json(out_dir / f"{slugify(record['name'])}.json", record)
        print(f"OK {record['name']}: {record['signature']}")

    manifest = {
        "engine_version": anchors_doc.get("engine_version"),
        "platform": anchors_doc.get("platform"),
        "anchor_count": len(records),
        "fixture": fixture_metadata(args.macho, data, text_section(data)),
        "anchors": records,
    }
    write_json(out_dir / "manifest.json", manifest)
    print(f"WROTE {out_dir / 'manifest.json'}")


if __name__ == "__main__":
    main()
