#!/usr/bin/env bash
# Runs a command; when it fails, writes a GitHub error annotation with the compiler errors from its output (or its
# last lines when there are none), so a failed build can be read from the run's annotations (and through the
# checks API) without the raw log.
# Usage: run_annotated.sh TITLE COMMAND [ARGUMENTS...]
set -uo pipefail

title=$1
shift
## Lines kept from the output in the annotation.
readonly keep_lines=60

log=$(mktemp)
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ]; then
   rm -f "$log"
   exit 0
fi
# Compiler and linker errors first; otherwise the end of the output.
out=$(grep -E -A3 "error:|Error [0-9]+|undefined reference|FAILED:" "$log" | head -n "$keep_lines")
if [ -z "$out" ]; then
   out=$(tail -n "$keep_lines" "$log")
fi
rm -f "$log"
out=${out//'%'/'%25'}
out=${out//$'\r'/}
out=${out//$'\n'/'%0A'}
echo "::error title=$title::$out"
exit "$status"
