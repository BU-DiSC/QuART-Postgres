# QuART in PostgreSQL's radix tree (VACUUM)

This fork of PostgreSQL (branch `master`, the fork's default branch, forked from master at 73dd7163c5, 19devel) is the
PostgreSQL part of the paper *QuART: Quick Ingestion to ART through Sortedness Awareness*,
Section 5.1. The main artifact, with the QuART implementation and the driver script
`experiments/5.1-postgres/run.sh` that runs this fork's harness, is at https://anonymous.4open.science/r/QuART-53DF.

| Where | What |
|---|---|
| `src/include/lib/quart.h` | QuART inside the radix-tree template `lib/radixtree.h`: `RT_SET_QUART`, the stail fast path (fp = the node of the current 256-key group, FP_INSERT / BRIDGE / OTHER classification, reset counter 16), and `RT_SET_QUART_BULK`, the grouped bulk loader for a strictly ascending key stream (gaps and offsets allowed) |
| `src/include/lib/radixtree.h` | `RT_USE_QUART` enables the two entry points in a template instance |
| `src/backend/access/common/tidstore.c` | VACUUM's TID store uses `RT_SET_QUART` when the GUC `vacuum_use_quart` is on (default), and writes every inserted block number to `quart_keydump_file` when that superuser-only GUC is set (the capture side of the experiment) |
| `src/backend/commands/vacuum.c`, `guc_parameters.dat`, `postgresql.conf.sample` | the two GUCs |
| `src/test/modules/test_radixtree/` | tests of the QuART paths |
| `quart_bench/` | the paper's capture-and-replay harness (Table 2): `capture.sh` records the key stream of a real VACUUM for workloads W1..W9, `replay` inserts it into the radix tree offline with ART, stail and guarded bulk loading, `run_replay.sh` + `report.py` produce the table; `results/replay.csv` is the run behind the paper. See its `README.md`. |

## Building

```sh
./configure --prefix=$PWD/install --without-readline && make -j && make install
```

(`--without-readline` only drops psql's line editing; add `--without-icu` if configure asks for ICU.)

## Running the experiment of Section 5.1 (Table 2)

Table 2 is produced by capture and replay: a real VACUUM's key stream is captured once, then replayed
offline into PostgreSQL's radix tree with three insertion paths (ART, the stail fast path, guarded
bulk loading), so that the difference isolates the insertion cost from VACUUM's I/O.

```sh
# 1. a private server from this build (it provides the quart_keydump_file setting)
install/bin/initdb -D pgdata
install/bin/pg_ctl -D pgdata -l logfile start
export PGHOST=localhost PGDATABASE=postgres           # plus PGPORT if you changed it

# 2. capture the key streams of workloads W1..W9 at each table size of the paper
quart_bench/capture.sh -n 250000000  -o quart_bench/keybins
quart_bench/capture.sh -n 1000000000 -o quart_bench/keybins
quart_bench/capture.sh -n 5000000000 -o quart_bench/keybins   # ~300 GB of disk, several hours

# 3. build the replay tool and replay every captured stream
make -C quart_bench
quart_bench/run_replay.sh quart_bench/keybins       # -> quart_bench/results/replay.csv
quart_bench/report.py quart_bench/results/replay.csv   # prints Table 2

install/bin/pg_ctl -D pgdata stop
```

Each `capture.sh` call builds a single-column table of the given size with an index, applies the
workload (update all rows, delete the first 25% or 75%, every 7th row, bands of rows, a mixed
update+delete, an aborted insert), and VACUUMs it with `quart_keydump_file` set, writing
`vackeys_<workload>_<scale>rows.bin`. Pass workload names after `-o DIR` to capture a subset, or a
smaller `-n` to try the pipeline (1M rows takes seconds). `run_replay.sh` keeps the best of
`REPS=10` runs in each of `TRIALS=3` trials and reports the median; every tree is verified key by key.
The run behind the paper is `quart_bench/results/replay.csv`; `quart_bench/README.md` has the details.

The same steps are driven from the main artifact by `experiments/5.1-postgres/run.sh`
(`PG_ROWS` selects the table sizes), which also writes the LaTeX body of Table 2.

---

*The upstream README follows.*

PostgreSQL Database Management System
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this version of PostgreSQL can be found at
<https://www.postgresql.org/docs/devel/>.  In particular, information
about building PostgreSQL from the source code can be found at
<https://www.postgresql.org/docs/devel/installation.html>.

The latest version of this software, and related software, may be
obtained at <https://www.postgresql.org/download/>.  For more information
look at our web site located at <https://www.postgresql.org/>.
