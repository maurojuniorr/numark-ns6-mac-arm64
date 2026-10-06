#!/bin/sh
set -eu

binary=${1:?usage: NS6USBWorkIntervalRegressionTest.sh <NumarkNS6 binary>}

imports=$(nm -u "$binary")
if printf '%s\n' "$imports" | grep -E '_AudioWorkIntervalCreate|_os_workgroup_(join|leave|interval_start|interval_update|interval_finish)' >/dev/null; then
    echo "FAIL: production USB driver imports AudioWorkInterval scheduling APIs" >&2
    exit 1
fi

echo "PASS: production USB driver has no AudioWorkInterval scheduling imports"
