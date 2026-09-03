#!/bin/bash
#
# run_with_diag.sh — 启动 csdaq 并自动采集诊断数据
#
# 用法:
#   ./scripts/run_with_diag.sh [sysconfig.yaml路径]
#
# 功能:
#   1. 后台启动 csdaq_main，stdout/stderr 存入日志文件
#   2. 后台轮询 /api/health，每 5 秒采样一次存入 jsonl 文件
#   3. Ctrl+C 同时停止 csdaq 和采样，打印日志路径
#
# 日志存储在程序目录下 logs/ 子目录:
#   logs/csdaq_YYYYMMDD_HHMMSS.log      — 控制台全量日志
#   logs/health_<run_id>_YYYYMMDD.jsonl — health 时序快照（按天分片）
#

set -uo pipefail
# 注意: 不用 set -e，因为 wait 返回子进程退出码，我们需要手动处理非零退出

# SSH 断连时控制终端会对进程组广播 SIGHUP，默认动作是 terminate，脚本和
# daq_main 会一起被杀。trap '' HUP 让脚本忽略 HUP，配合下面 daq_main 启动时
# 加 nohup（让子进程也 ignore HUP），整条链路就不会因为 ssh 抖一下而死。
# 停止仍然走正常路径：scripts/cmd_stop.sh 发 HTTP stop+terminate，daq_main
# 干净退出后 wait 返回，cleanup 正常收尾。
trap '' HUP

# ---------- 定位程序目录 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DAQ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 找可执行文件: 优先 build/csdaq_main，其次 DAQ_DIR/csdaq_main
CSDAQ_BIN=""
for candidate in \
    "$DAQ_DIR/build/daq_main" \
    "$DAQ_DIR/daq_main" \
    "$DAQ_DIR/build/csdaq_main" \
    "$DAQ_DIR/csdaq_main"; do
    if [[ -x "$candidate" ]]; then
        CSDAQ_BIN="$candidate"
        break
    fi
done
if [[ -z "$CSDAQ_BIN" ]]; then
    echo "ERROR: daq_main not found in $DAQ_DIR/build/ or $DAQ_DIR/"
    echo "       Please build first (cmake --build build)."
    exit 1
fi

# ---------- sysconfig 路径 ----------
SYSCONFIG="${1:-}"
if [[ -n "$SYSCONFIG" ]]; then
    SYSCONFIG_ARG="$SYSCONFIG"
else
    SYSCONFIG_ARG=""
    echo "INFO: No sysconfig specified, csdaq_main will use its default path."
fi

# ---------- HTTP 端口（默认 8080，可通过环境变量覆盖） ----------
HTTP_PORT="${CSDAQ_HTTP_PORT:-8080}"
HEALTH_URL="http://localhost:${HTTP_PORT}/api/health"

# ---------- 日志目录 ----------
RUN_ID="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="$DAQ_DIR/logs"
mkdir -p "$LOG_DIR"

CONSOLE_LOG="$LOG_DIR/csdaq_${RUN_ID}.log"
HEALTH_LOG_PATTERN="$LOG_DIR/health_${RUN_ID}_YYYYMMDD.jsonl"

echo "============================================"
echo " CSDAQ Diagnostic Runner"
echo "============================================"
echo " Binary:       $CSDAQ_BIN"
echo " Sysconfig:    ${SYSCONFIG_ARG:-<default>}"
echo " HTTP port:    $HTTP_PORT"
echo " Console log:  $CONSOLE_LOG"
echo " Run ID:       $RUN_ID"
echo " Health logs:  $HEALTH_LOG_PATTERN"
echo " Health poll:  every 5s"
echo "============================================"

# ---------- 清理函数 ----------
CSDAQ_PID=""
HEALTH_PID=""
CSDAQ_EXIT_CODE=0

cleanup() {
    local exit_code="${1:-$CSDAQ_EXIT_CODE}"
    echo ""
    echo ">>> Stopping..."

    # 先优雅停止: 调用 stop + terminate API
    if curl -s -o /dev/null -w '' --max-time 2 \
         -X POST "http://localhost:${HTTP_PORT}/api/cmd/stop" -d '{}' 2>/dev/null; then
        sleep 1
        curl -s -o /dev/null --max-time 2 \
             -X POST "http://localhost:${HTTP_PORT}/api/cmd/terminate" -d '{}' 2>/dev/null || true
        sleep 1
    fi

    # 停 health 采样
    if [[ -n "$HEALTH_PID" ]] && kill -0 "$HEALTH_PID" 2>/dev/null; then
        kill "$HEALTH_PID" 2>/dev/null || true
        wait "$HEALTH_PID" 2>/dev/null || true
    fi

    # 如果 csdaq 还在运行，发 SIGTERM
    if [[ -n "$CSDAQ_PID" ]] && kill -0 "$CSDAQ_PID" 2>/dev/null; then
        kill "$CSDAQ_PID" 2>/dev/null || true
        wait "$CSDAQ_PID" 2>/dev/null || true
    fi

    echo ""
    echo "============================================"
    if [[ "$exit_code" -ne 0 ]]; then
        echo " daq_main exited with code $exit_code (ABNORMAL)"
    else
        echo " Stopped normally."
    fi
    echo " Logs saved:"
    echo "   Console: $CONSOLE_LOG"
    echo "   Health:  $HEALTH_LOG_PATTERN"
    CONSOLE_LINES=$(wc -l < "$CONSOLE_LOG" 2>/dev/null || echo 0)
    HEALTH_LINES=$(wc -l "$LOG_DIR"/health_"$RUN_ID"_*.jsonl 2>/dev/null | awk 'END {print $1 + 0}')
    echo "   Console lines: $CONSOLE_LINES"
    echo "   Health samples: $HEALTH_LINES"
    echo "============================================"
    exit "$exit_code"
}

trap 'cleanup 0' INT TERM

# ---------- 启动 csdaq ----------
echo ">>> Starting csdaq_main..."
if [[ -n "$SYSCONFIG_ARG" ]]; then
    nohup "$CSDAQ_BIN" "$SYSCONFIG_ARG" >> "$CONSOLE_LOG" 2>&1 &
else
    nohup "$CSDAQ_BIN" >> "$CONSOLE_LOG" 2>&1 &
fi
CSDAQ_PID=$!
echo "    PID: $CSDAQ_PID"

# 等待 HTTP 服务就绪（最多 15 秒）
echo ">>> Waiting for HTTP API to be ready..."
READY=false
for i in $(seq 1 30); do
    if curl -s -o /dev/null --max-time 1 "$HEALTH_URL" 2>/dev/null; then
        READY=true
        break
    fi
    # 检查进程是否还活着
    if ! kill -0 "$CSDAQ_PID" 2>/dev/null; then
        echo "ERROR: csdaq_main exited unexpectedly. Check log: $CONSOLE_LOG"
        tail -20 "$CONSOLE_LOG"
        exit 1
    fi
    sleep 0.5
done

if [[ "$READY" != "true" ]]; then
    echo "ERROR: HTTP API not ready after 15s. Check log: $CONSOLE_LOG"
    tail -20 "$CONSOLE_LOG"
    cleanup 1
fi
echo "    HTTP API ready."

# ---------- 启动 health 采样 ----------
echo ">>> Starting health sampler (every 5s) -> $HEALTH_LOG_PATTERN"
(
    while true; do
        # 采样 health，加上本地时间戳
        RESPONSE=$(curl -s --max-time 3 "$HEALTH_URL" 2>/dev/null || echo "")
        if [[ -n "$RESPONSE" ]]; then
            TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)
            DAY=$(date +%Y%m%d)
            HEALTH_LOG="$LOG_DIR/health_${RUN_ID}_${DAY}.jsonl"
            # 用 stdin 传 health JSON，避免 /api/health 变大后触发 argv 长度限制。
            printf '%s' "$RESPONSE" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(json.dumps({'ts': sys.argv[1], 'health': d}))
except Exception:
    sys.exit(1)
" "$TS" >> "$HEALTH_LOG" 2>/dev/null || \
            echo "{\"ts\":\"$TS\",\"raw\":\"health_parse_error\"}" >> "$HEALTH_LOG"
        fi
        sleep 5
    done
) &
HEALTH_PID=$!

echo ""
echo "============================================"
echo " CSDAQ is running. Next steps:"
echo "   1. Initialize:  curl -X POST http://localhost:${HTTP_PORT}/api/cmd/initialize -d '{}'"
echo "   2. Start:       curl -X POST http://localhost:${HTTP_PORT}/api/cmd/start -d '{}'"
echo "   3. Monitor:     tail -f $CONSOLE_LOG"
echo "   4. Health:      curl -s http://localhost:${HTTP_PORT}/api/health | python3 -m json.tool"
echo "   5. Stop:        Ctrl+C (or run scripts/cmd_stop.sh)"
echo "============================================"
echo ""
echo ">>> Waiting... Press Ctrl+C to stop."

# 等待 csdaq 进程退出，保留退出码
wait "$CSDAQ_PID" 2>/dev/null
CSDAQ_EXIT_CODE=$?
echo ">>> daq_main exited (code=$CSDAQ_EXIT_CODE)."
cleanup "$CSDAQ_EXIT_CODE"
