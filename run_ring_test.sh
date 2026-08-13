#!/usr/bin/env bash

set -uo pipefail

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  echo "请使用 bash run_ring_test.sh 运行，不要使用 source。" >&2
  return 2
fi

mkdir -p logs/full logs/filtered

run_id=$(date +%Y%m%d_%H%M%S)
full_log="logs/full/${run_id}.log"
filtered_log="logs/filtered/${run_id}.log"

echo "完整日志: ${full_log}"
echo "过滤日志: ${filtered_log}"

stdbuf -oL -eL ./test_prj "$@" 2>&1 |
  tee "$full_log" |
  awk '!/^[[:space:]]*PERF_RING_BUFFER[[:space:]]+node_type=/ {
    print
    fflush()
  }' |
  tee "$filtered_log"

test_status=${PIPESTATUS[0]}
echo "测试退出码: ${test_status}"
exit "$test_status"
