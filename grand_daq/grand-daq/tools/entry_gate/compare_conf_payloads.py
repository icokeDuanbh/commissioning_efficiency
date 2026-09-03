#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def load_dump(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser(description="Compare new vs old CONF payload dumps.")
    ap.add_argument("--new", required=True, help="new dump json")
    ap.add_argument("--old", required=True, help="old dump json")
    ap.add_argument("--out", required=True, help="markdown report path")
    ap.add_argument("--summary-json", help="optional summary json path")
    args = ap.parse_args()

    new_dump = load_dump(Path(args.new))
    old_dump = load_dump(Path(args.old))

    new_map = {int(item["du_id"]): item for item in new_dump["dus"]}
    old_map = {int(item["du_id"]): item for item in old_dump["dus"]}

    all_ids = sorted(set(new_map) | set(old_map))
    only_new = [du for du in all_ids if du in new_map and du not in old_map]
    only_old = [du for du in all_ids if du in old_map and du not in new_map]
    mismatches = []
    matched_equal = []

    for du_id in all_ids:
        if du_id not in new_map or du_id not in old_map:
            continue
        new_item = new_map[du_id]
        old_item = old_map[du_id]
        if (
            new_item["conf_payload_size"] != old_item["conf_payload_size"]
            or new_item["conf_payload_hex"] != old_item["conf_payload_hex"]
        ):
            mismatches.append(
                {
                    "du_id": du_id,
                    "new_size": new_item["conf_payload_size"],
                    "old_size": old_item["conf_payload_size"],
                    "new_prefix": new_item["conf_payload_hex"][:64],
                    "old_prefix": old_item["conf_payload_hex"][:64],
                }
            )
        else:
            matched_equal.append(du_id)

    lines = []
    lines.append("# CONF 对照结果")
    lines.append("")
    lines.append("## 输入")
    lines.append(f"- new dump: `{Path(args.new)}`")
    lines.append(f"- old dump: `{Path(args.old)}`")
    lines.append(f"- new sysconfig: `{new_dump['sysconfig']}`")
    lines.append(f"- old sysconfig: `{old_dump['sysconfig']}`")
    lines.append(f"- new sidecar: `{new_dump['addr_map']}`, `{new_dump['readable_conf']}`")
    lines.append(f"- old sidecar: `{old_dump['addr_map']}`, `{old_dump['readable_conf']}`")
    lines.append("")
    lines.append("## 总结")
    lines.append(f"- new DU 数：`{new_dump['du_count']}`")
    lines.append(f"- old DU 数：`{old_dump['du_count']}`")
    lines.append(f"- payload 完全一致的 DU 数：`{len(matched_equal)}`")
    lines.append(f"- payload 不一致的 DU 数：`{len(mismatches)}`")
    lines.append(f"- 仅在 new 出现的 DU：`{only_new}`")
    lines.append(f"- 仅在 old 出现的 DU：`{only_old}`")
    lines.append("")
    if mismatches:
        lines.append("## 不一致 DU")
        for item in mismatches:
            lines.append(
                f"- du_id=`{item['du_id']}` new_size=`{item['new_size']}` old_size=`{item['old_size']}` "
                f"new_prefix=`{item['new_prefix']}` old_prefix=`{item['old_prefix']}`"
            )
    else:
        lines.append("## 不一致 DU")
        lines.append("- 无")
    lines.append("")
    if matched_equal:
        preview = matched_equal[:10]
        lines.append("## 一致 DU 预览")
        lines.append(f"- `{preview}`")

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")

    if args.summary_json:
        summary = {
            "new_du_count": new_dump["du_count"],
            "old_du_count": old_dump["du_count"],
            "matched_equal_count": len(matched_equal),
            "mismatch_count": len(mismatches),
            "only_new": only_new,
            "only_old": only_old,
            "mismatches": mismatches,
        }
        Path(args.summary_json).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
