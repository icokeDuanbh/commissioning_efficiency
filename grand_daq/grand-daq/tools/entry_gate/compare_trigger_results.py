#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def normalize_result(item):
    return {
        "tm_id": item.get("tm_id"),
        "triggered": item.get("triggered"),
        "du_count": item.get("du_count"),
        "timestamp_ns": item.get("timestamp_ns"),
        "du_ids": item.get("du_ids"),
        "trigger_timestamps": item.get("trigger_timestamps"),
        "dotrigger_payload_hex": item.get("dotrigger_payload_hex"),
    }


def main():
    ap = argparse.ArgumentParser(description="Compare replayed trigger results.")
    ap.add_argument("--new", required=True)
    ap.add_argument("--old", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--summary-json", help="optional summary json")
    args = ap.parse_args()

    new_doc = load_json(Path(args.new))
    old_doc = load_json(Path(args.old))

    new_results = [normalize_result(item) for item in new_doc.get("results", [])]
    old_results = [normalize_result(item) for item in old_doc.get("results", [])]

    mismatches = []
    count = max(len(new_results), len(old_results))
    for idx in range(count):
        new_item = new_results[idx] if idx < len(new_results) else None
        old_item = old_results[idx] if idx < len(old_results) else None
        if new_item != old_item:
            mismatches.append({"index": idx, "new": new_item, "old": old_item})

    lines = []
    lines.append("# Trigger replay 对照结果")
    lines.append("")
    lines.append("## 输入")
    lines.append(f"- new replay: `{args.new}`")
    lines.append(f"- old replay: `{args.old}`")
    lines.append("")
    lines.append("## 总结")
    lines.append(f"- new result 数：`{len(new_results)}`")
    lines.append(f"- old result 数：`{len(old_results)}`")
    lines.append(f"- mismatch 数：`{len(mismatches)}`")
    lines.append("")
    lines.append("## mismatch")
    if not mismatches:
        lines.append("- 无")
    else:
        for item in mismatches[:20]:
            lines.append(f"- index=`{item['index']}`")
            lines.append(f"  - new: `{item['new']}`")
            lines.append(f"  - old: `{item['old']}`")

    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    if args.summary_json:
        Path(args.summary_json).write_text(
            json.dumps(
                {
                    "new_count": len(new_results),
                    "old_count": len(old_results),
                    "mismatch_count": len(mismatches),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
