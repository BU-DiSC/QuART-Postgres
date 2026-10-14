# quart_bench — PostgreSQL VACUUM capture-and-replay

Reproduces the PostgreSQL VACUUM experiment of the QuART paper (§5.1,
Table "PostgreSQL VACUUM index insert results"). Insertion cost is measured by
**replay**, not by timing VACUUM: a real VACUUM's TID-store key stream is
captured once, then replayed as pure insertions into PostgreSQL's own radix
tree (`src/include/lib/radixtree.h`) outside the server — no I/O, WAL, buffer
management or index scans. The same keys go through the same tree code, so the
difference isolates the insertion path:

| Path    | Function                | Paper column            |
|---------|-------------------------|-------------------------|
| `art`   | `RT_SET`                | baseline (ART)          |
| `stail` | `RT_SET_QUART`          | stail over ART          |
| `bulk`  | guard + `RT_SET_QUART_BULK`, else `RT_SET_QUART` | Bulk load over ART |

The guard is one linear pass that admits a strictly ascending stream to the
bulk loader; any other stream falls back to the stail fast path. Gaps and
offsets are allowed: the loader groups keys by `key >> 8` and builds each
group's leaf-level node at its final size (node4 to node256), so gaps only make
groups smaller. A VACUUM stream is always ascending, so every workload is bulk
loaded.

## Workloads

Each workload builds a single-column table of `ROWS` rows (`int4`, or `int8`
when ids would overflow it; 226 tuples per 8 kB page either way) with an index
on `id`, creates dead tuples, and VACUUMs it. VACUUM scans the heap in block
order, so every stream is ascending; workloads differ in which blocks hold
dead tuples.

| Id | Name       | SQL                                                    | Density |
|----|------------|--------------------------------------------------------|---------|
| W1 | `update`   | `UPDATE SET id = id - 1`                               | 1.00 |
| W2 | `range25`  | `DELETE WHERE id <= ROWS/4`                            | 1.00 |
| W3 | `range75`  | `DELETE WHERE id <= 3*ROWS/4`                          | 1.00 |
| W4 | `striped7` | `DELETE WHERE id % 7 = 0`                              | 1.00 |
| W5 | `banded`   | `DELETE WHERE (id/1000) % 2 = 0`                       | 0.61 |
| W6 | `sparse`   | `DELETE WHERE (id/1000) % 5 = 0`                       | 0.25 |
| W7 | `wideband` | `DELETE WHERE (id/10000) % 2 = 0`                      | 0.51 |
| W8 | `mixed`    | `UPDATE ... WHERE id <= ROWS/2` + `DELETE WHERE id > ROWS/2` | 1.00 |
| W9 | `abort`    | `INSERT ROWS/4 new rows`, then `ROLLBACK` (offset into high blocks) | 1.00 |

Density is the gap-free fraction of the inserted block range. The paper uses
`ROWS` = 250M, 1B and 5B.

## Running

1. **Build and start a server from this tree** (it provides the superuser-only
   `quart_keydump_file` setting that `TidStoreSetBlockOffsets` writes keys to):

   ```sh
   ./configure --prefix=$PWD/install && make -j && make install
   install/bin/initdb -D pgdata && install/bin/pg_ctl -D pgdata -l logfile start
   ```

2. **Capture** the key streams (connects with the usual `PG*` variables as a
   superuser; needs disk for the table and its index — about 300 GB at 5B rows):

   ```sh
   quart_bench/capture.sh -n 250000000 -o quart_bench/keybins
   quart_bench/capture.sh -n 1000000000 -o quart_bench/keybins
   quart_bench/capture.sh -n 5000000000 -o quart_bench/keybins
   ```

   Each run writes `vackeys_<workload>_<scale>rows.bin` (little-endian
   `uint64` block numbers, in insertion order). Pass workload names to capture
   a subset; `-m` sets `maintenance_work_mem` (default 4GB, a single pass).

3. **Build the replay tool** against this tree's headers (`PG_BUILD` only for a
   VPATH build):

   ```sh
   make -C quart_bench [PG_BUILD=/path/to/build]
   ```

4. **Replay and report**:

   ```sh
   quart_bench/run_replay.sh quart_bench/keybins     # -> quart_bench/results/replay.csv
   quart_bench/report.py quart_bench/results/replay.csv
   ```

   For every path, each of `TRIALS` (default 3) trials keeps the fastest of
   `REPS` (default 10) runs, with the order of the paths rotated per run; the
   median trial is reported. Every tree is checked key by key against the
   stream (`verify` column), so a corrupted tree cannot produce a timing.

`replay` can also be run on a single stream:
`quart_bench/replay -H [-r REPS] [-t TRIALS] keys.bin`.

## Output

`results/replay.csv` has one row per stream: `keys`, `min_key`/`max_key`,
`density`, whether the guard admits it (`sorted`), ns per key for each path
(`art_ns`, `stail_ns`, `bulk_ns`), the speedups over `art`, the path the guarded
loader took (`bulk_path`), the guard's own cost (`guard_ns`) and `verify`.
The checked-in file is a reference run of the 27 paper streams; numbers depend
on the machine.
