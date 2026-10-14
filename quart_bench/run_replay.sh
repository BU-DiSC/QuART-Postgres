#!/usr/bin/env bash
#
# run_replay.sh - replay every captured key stream and collect one CSV
#
# Usage: run_replay.sh KEYBIN_DIR [OUT_CSV]
#   KEYBIN_DIR  directory of vackeys_<workload>_<scale>rows*.bin (capture.sh)
#   OUT_CSV     default: results/replay.csv
# Environment: REPS (default 10) and TRIALS (default 3) are passed to replay:
# each trial keeps the best of REPS runs per path; the median trial is kept.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KEYBINS=${1:?usage: run_replay.sh KEYBIN_DIR [OUT_CSV]}
OUT=${2:-$HERE/results/replay.csv}
REPS=${REPS:-10}
TRIALS=${TRIALS:-3}

if [[ ! -x $HERE/replay ]]; then
	echo "build the replay tool first: make -C $HERE" >&2
	exit 1
fi
mkdir -p "$(dirname "$OUT")"

echo "scale,workload,keys,min_key,max_key,density,sorted,art_ns,stail_ns,stail_speedup,bulk_path,bulk_ns,bulk_speedup,guard_ns,verify" > "$OUT"

status=0
for f in "$KEYBINS"/vackeys_*rows*.bin; do
	name=$(basename "$f")
	if ! [[ $name =~ ^vackeys_([a-z0-9]+)_([0-9.]+[MB]?)rows ]]; then
		echo "skipping $name (not vackeys_<workload>_<scale>rows*.bin)" >&2
		continue
	fi
	wl=${BASH_REMATCH[1]}
	scale=${BASH_REMATCH[2]}
	echo "[$(date +%T)] $scale $wl" >&2
	if row=$("$HERE/replay" -r "$REPS" -t "$TRIALS" "$f"); then :; else status=1; fi
	echo "$scale,$wl,${row#*,}" >> "$OUT"
done

echo "wrote $OUT" >&2
exit $status
