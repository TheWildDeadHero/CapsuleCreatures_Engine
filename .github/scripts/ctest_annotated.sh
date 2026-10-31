#!/usr/bin/env bash
# Runs ctest for a preset; when tests fail, writes one GitHub error annotation per failed test with the end of its
# output, so failures can be read from the run's annotations (and through the checks API) without the raw log.
# Usage: ctest_annotated.sh PRESET [extra ctest arguments]
set -euo pipefail

preset=$1
shift
## Output lines kept per failed test in its annotation.
readonly tail_lines=40

if ctest --preset "$preset" --output-on-failure "$@"; then
   exit 0
fi
failed="build/$preset/Testing/Temporary/LastTestsFailed.log"
if [ ! -f "$failed" ]; then
   echo "::error title=ctest ($preset)::ctest failed before running any test (no LastTestsFailed.log)"
   exit 1
fi
while IFS=: read -r _number name; do
   [ -n "$name" ] || continue
   # Re-run the one test to capture its own output; annotations take %0A for new lines.
   out=$(ctest --preset "$preset" --output-on-failure -R "^${name}\$" 2>&1 | tail -n "$tail_lines" || true)
   out=${out//'%'/'%25'}
   out=${out//$'\r'/}
   out=${out//$'\n'/'%0A'}
   echo "::error title=ctest ($preset) $name::$out"
done < "$failed"
exit 1
