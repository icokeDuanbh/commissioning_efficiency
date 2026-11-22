#!/usr/bin/env bash
set -euo pipefail
SECONDS=0
trap 'status=$?; printf "总耗时：%d 秒（退出码：%d）\n" "$SECONDS" "$status" >&2' EXIT

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if [[ ${CONDA_DEFAULT_ENV:-} != grandlib-root || -z ${CONDA_PREFIX:-} ]]; then
    conda_executable="${CONDA_EXE:-$(command -v conda || true)}"
    if [[ -z "$conda_executable" ]]; then
        echo "找不到 conda，请先在当前终端加载 Conda。" >&2
        exit 1
    fi
    conda_base="$("$conda_executable" info --base)"
    # Conda activation hooks may refer to unset variables.
    set +u
    source "$conda_base/etc/profile.d/conda.sh"
    conda activate grandlib-root
    set -u
fi

python_bin=/sharegp/apps/python/3.14.7/bin/python3
python_lib=/sharegp/apps/python/3.14.7/lib
cxx_lib="$CONDA_PREFIX/lib/libstdc++.so.6"
if [[ ! -x "$python_bin" || ! -r "$cxx_lib" ]]; then
    printf '请检查 Python 和 C++ 库路径：\n%s\n%s\n' "$python_bin" "$cxx_lib" >&2
    exit 1
fi
if ! strings "$cxx_lib" | grep -Fx 'GLIBCXX_3.4.32' >/dev/null; then
    echo "$cxx_lib 缺少 GLIBCXX_3.4.32" >&2
    exit 1
fi

LD_LIBRARY_PATH="$python_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
LD_PRELOAD="$cxx_lib${LD_PRELOAD:+:$LD_PRELOAD}" \
    "$python_bin" -u run_event_from_json.py "$@"
