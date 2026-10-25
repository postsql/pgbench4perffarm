# pgbench4perffarm

Standalone extraction of the next-generation PostgreSQL `pgbench` client benchmark driver (`src/bin/pgbench`), `contrib/pgbench` server-side C extension (`contrib/pgbench`), shared statistical/data-generation library (`src/common/pgbench_funcs.c`), multi-domain workload suites (`workloads/pgbench-*`), and web telemetry viewer (`viewer/`) from patch set v6.

Builds and runs out-of-the-box via `PGXS` against all officially supported PostgreSQL versions (**PostgreSQL 14, 15, 16, 17, 18, 19, and 20devel**).

> **Disclaimer**: All `pgbench-*` workload suites (`pgbench-b`, `pgbench-c`, `pgbench-e`, `pgbench-h`, `pgbench-ds`, `pgbench-htap`, `pgbench-graph`, `pgbench-vec`) are **non-comparable, inspired-by workloads** for PostgreSQL testing and development. None of these results are official TPC, LDBC, or VecBench benchmark results, nor are they comparable to any official TPC, LDBC, or VecBench published results.

## Repository Layout

- `src/bin/pgbench/` — `pgbench` client driver supporting JSON/JSONC configuration (`--config`, `--dump-config`), heterogeneous client groups (`--group`), shared lock-free reservoirs (`--reservoir`), phased barriers (`\phase_barrier`), causal watermarks, result validation (`\validate_result`), Recall@K / NDCG@K (`\record_recall`), HDR latency histograms, SLA enforcement (`--sla`), JSON telemetry reports (`--json-report`), and direct PostgreSQL telemetry logging (`--log-db`).
- `contrib/pgbench/` — Server-side C extension (`CREATE EXTENSION pgbench;`) exposing fast C implementations of uniform/gaussian/exponential/zipfian/poisson random distributions, FNV-1a and Murmur2 hashing, domain permutations (`pgbench.permute`), NURand (`pgbench.nurand`), syllable/string/bytea generators, date arithmetic, multi-modal dense/sparse vector generators and distance metrics, and R-MAT / LDBC correlated graph edge generators in the `pgbench` schema.
- `src/common/pgbench_funcs.c` & `src/include/common/pgbench_funcs.h` — Shared frontend/backend statistical and synthetic data generation library with compatibility shims for PostgreSQL 14 through 20devel.
- `workloads/` (`src/bin/pgbench/workloads/`) — 8 ready-to-run workload suites:
  - `workloads/pgbench-b/` — Simple update-intensive OLTP (`pgbench-b`)
  - `workloads/pgbench-c/` — 9-table wholesale supplier OLTP (`pgbench-c`: SQL, SQL_UD, PL/pgSQL)
  - `workloads/pgbench-e/` — 33-table financial brokerage OLTP (`pgbench-e`)
  - `workloads/pgbench-h/` — 8-table decision support analytics (`pgbench-h`: power & throughput)
  - `workloads/pgbench-ds/` — 24-table retail/web/catalog decision support (`pgbench-ds`)
  - `workloads/pgbench-htap/` — Hybrid transactional/analytical processing (`pgbench-htap`)
  - `workloads/pgbench-graph/` — Social network graph (`pgbench-graph`: SQL, SQL_UD, PGQ, plPGQ)
  - `workloads/pgbench-vec/` — Vector similarity search (`pgbench-vec`: hybrid search & index aging)
- `viewer/` (`src/bin/pgbench/viewer/`) — Standalone zero-dependency Python (`bottle.py`) + WebAssembly (`pgbench_chart.wasm`) web telemetry viewer (`pgbench_viewer.py`).
- `doc/src/sgml/` — SGML reference documentation (`ref/pgbench.sgml` and `pgbench-ext.sgml`).

## Building and Installing

Specify `PG_CONFIG` pointing to your target PostgreSQL installation (PostgreSQL 14, 15, 16, 17, 18, 19, or 20devel):

```bash
make -j$(nproc) PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

To run the `contrib/pgbench` SQL regression test suite against a running PostgreSQL server:

```bash
make installcheck PG_CONFIG=/path/to/pg_config
```

## Quick Start (`-s 10 -c 10 -T 10`)

```bash
# 1. Initialize a workload suite at scale factor 10
pgbench -i -s 10 -D scale=10 --config=workloads/pgbench-c/config.json <dbname>

# 2. Run a 10-second benchmark at concurrency 10
pgbench --config=workloads/pgbench-c/config.json -s 10 -D scale=10 -c 10 -j 4 -T 10 <dbname>
```

## License

Released under the [PostgreSQL License](COPYRIGHT).
