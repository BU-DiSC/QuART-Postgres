#!/usr/bin/env bash
#
# capture.sh - capture the VACUUM key streams of the QuART paper workloads
#
# For each workload: build a fresh single-column table of ROWS rows with a
# secondary index, create dead tuples with the workload's SQL, then VACUUM it
# with quart_keydump_file set, so every block number VACUUM inserts into its
# TID store is written to OUT/vackeys_<workload>_<scale>rows.bin.
#
# Usage: capture.sh -n ROWS -o OUT [-m MAINT_WORK_MEM] [-k] [workload ...]
#   -n ROWS   table size (paper: 250000000, 1000000000, 5000000000)
#   -o OUT    output directory for the key files
#   -m MEM    maintenance_work_mem for the VACUUM (default 4GB, one pass)
#   -k        keep the table after the last workload
# Workloads (default: all, in paper order W1..W9):
#   update range25 range75 striped7 banded sparse wideband mixed abort
#
# Connects with psql using the standard PG* environment variables; needs a
# superuser (quart_keydump_file is superuser-only) on a server built from
# this tree.  Only the key stream matters: timings are taken offline by
# run_replay.sh, never from this VACUUM.

set -euo pipefail

ROWS=
OUT=
MWM=4GB
KEEP=0
ALL_WORKLOADS=(update range25 range75 striped7 banded sparse wideband mixed abort)

while getopts "n:o:m:k" opt; do
	case $opt in
		n) ROWS=$OPTARG ;;
		o) OUT=$OPTARG ;;
		m) MWM=$OPTARG ;;
		k) KEEP=1 ;;
		*) sed -n '3,22p' "$0"; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
WORKLOADS=("${@:-${ALL_WORKLOADS[@]}}")

if [[ -z $ROWS || -z $OUT ]] || ! [[ $ROWS =~ ^[0-9]+$ ]]; then
	sed -n '3,22p' "$0"
	exit 1
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# scale label used in file names: 250M, 1B, 5B, ...
if (( ROWS % 1000000000 == 0 )); then
	SCALE="$((ROWS / 1000000000))B"
elif (( ROWS % 1000000 == 0 )); then
	SCALE="$((ROWS / 1000000))M"
else
	SCALE="$ROWS"
fi

# int4 while every id (abort inserts up to ROWS * 1.25) fits, else int8;
# both pad to the same tuple size, 226 tuples per 8 kB page
if (( ROWS + ROWS / 4 > 2147483647 )); then IDTYPE=int8; else IDTYPE=int4; fi

PSQL=(psql -X -q -v ON_ERROR_STOP=1)

workload_sql() {
	local n=$1
	case $2 in
		update)   echo "UPDATE quart_vac SET id = id - 1;" ;;
		range25)  echo "DELETE FROM quart_vac WHERE id <= $((n / 4));" ;;
		range75)  echo "DELETE FROM quart_vac WHERE id <= $((n / 4 * 3));" ;;
		striped7) echo "DELETE FROM quart_vac WHERE id % 7 = 0;" ;;
		banded)   echo "DELETE FROM quart_vac WHERE (id / 1000) % 2 = 0;" ;;
		sparse)   echo "DELETE FROM quart_vac WHERE (id / 1000) % 5 = 0;" ;;
		wideband) echo "DELETE FROM quart_vac WHERE (id / 10000) % 2 = 0;" ;;
		mixed)    echo "UPDATE quart_vac SET id = id - 1 WHERE id <= $((n / 2));"
		          echo "DELETE FROM quart_vac WHERE id > $((n / 2));" ;;
		abort)    echo "BEGIN;"
		          echo "INSERT INTO quart_vac SELECT generate_series($((n + 1)), $((n + n / 4)));"
		          echo "ROLLBACK;" ;;
		*) echo "unknown workload: $2" >&2; return 1 ;;
	esac
}

for wl in "${WORKLOADS[@]}"; do
	sql=$(workload_sql "$ROWS" "$wl")
	file="$OUT/vackeys_${wl}_${SCALE}rows.bin"
	echo "[$(date +%T)] $wl ($SCALE rows): building table"

	"${PSQL[@]}" <<-EOF
		DROP TABLE IF EXISTS quart_vac;
		CREATE UNLOGGED TABLE quart_vac (id $IDTYPE) WITH (autovacuum_enabled = off);
		INSERT INTO quart_vac SELECT generate_series(1, $ROWS);
		CREATE INDEX quart_vac_id_idx ON quart_vac (id);
	EOF

	echo "[$(date +%T)] $wl: $(echo "$sql" | tr '\n' ' ')"
	"${PSQL[@]}" <<-EOF
		$sql
		SET maintenance_work_mem = '$MWM';
		SET quart_keydump_file = '$file';
		VACUUM (PARALLEL 0) quart_vac;
		RESET quart_keydump_file;
	EOF

	echo "[$(date +%T)] $wl: $(( $(stat -c %s "$file") / 8 )) keys -> $file"
done

if (( ! KEEP )); then
	"${PSQL[@]}" -c "DROP TABLE IF EXISTS quart_vac;"
fi
