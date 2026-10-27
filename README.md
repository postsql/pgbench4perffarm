# pgbench4perffarm

Standalone extraction of the next-generation PostgreSQL `pgbench` client benchmark driver (`src/bin/pgbench`), `contrib/pgbench` server-side C extension (`contrib/pgbench`), shared statistical/data-generation library (`src/common/pgbench_funcs.c`), multi-domain workload suites (`workloads/pgbench-*`), and web telemetry viewer (`viewer/`) from patch set v6.

Builds and runs out-of-the-box via `PGXS` against all officially supported PostgreSQL versions (**PostgreSQL 14, 15, 16, 17, 18, 19, and 20devel**).

> **Disclaimer**: All `pgbench-*` workload suites (`pgbench-b`, `pgbench-c`, `pgbench-e`, `pgbench-h`, `pgbench-ds`, `pgbench-htap`, `pgbench-graph`, `pgbench-vec`) are **non-comparable, inspired-by workloads** for PostgreSQL testing and development. None of these results are official TPC, LDBC, or VecBench benchmark results, nor are they comparable to any official TPC, LDBC, or VecBench published results.

## Repository Layout

- `src/bin/pgbench/` — `pgbench` client driver supporting JSON/JSONC configuration (`--config`, `--dump-config`), heterogeneous client groups (`--group`), shared lock-free reservoirs (`--reservoir`), phased barriers (`\phase_barrier`), causal watermarks, result validation (`\validate_result`), Recall@K / NDCG@K (`\record_recall`), HDR latency histograms, SLA enforcement (`--sla`), JSON telemetry reports (`--json-report`), and direct PostgreSQL telemetry logging (`--log-db`).
- `contrib/log2_hist/` — Server-side C extension (`CREATE EXTENSION log2_hist;`) providing a 64-bucket RLE-compressed `log2(V+1)` histogram base type (`log2_hist`) with `'[(nr-of-zeros){count[,count]*}]'` serialization, 0-based (`0..63`) raw subscripting and slicing, live-range `bigint[]` casting, branchless/AVX-512 saturating `uint64` addition, B-tree/Hash operator classes, percentile estimation, and parallel-safe aggregates ([`contrib/log2_hist/README.md`](contrib/log2_hist/README.md)).
- `contrib/pgbench/` — Server-side C extension (`CREATE EXTENSION pgbench;`) exposing fast C implementations of uniform/gaussian/exponential/zipfian/poisson random distributions, FNV-1a and Murmur2 hashing, domain permutations (`pgbench.permute`), NURand (`pgbench.nurand`), syllable/string/bytea generators, date arithmetic, multi-modal dense/sparse vector generators and distance metrics, and R-MAT / LDBC correlated graph edge generators in the `pgbench` schema.
- `src/common/pgbench_funcs.c` & `src/include/common/pgbench_funcs.h` — Shared frontend/backend statistical and synthetic data generation library with compatibility shims for PostgreSQL 14 through 20devel.
- `workloads/` (`src/bin/pgbench/workloads/`) — 8 ready-to-run workload suites:
  - `workloads/pgbench-b/` — 4-table simple update-intensive OLTP (`pgbench-b`: `pgbench_b.pgbench.json`)
  - `workloads/pgbench-c/` — 9-table wholesale supplier OLTP (`pgbench-c`: `pgbench_c.pgbench.json`)
  - `workloads/pgbench-e/` — 16-table financial brokerage OLTP (`pgbench-e`: `pgbench_e.pgbench.json`)
  - `workloads/pgbench-h/` — 8-table decision support analytics (`pgbench-h`: `pgbench_h.pgbench.json`)
  - `workloads/pgbench-ds/` — 13-table retail/web/catalog decision support (`pgbench-ds`: `pgbench_ds.pgbench.json`)
  - `workloads/pgbench-htap/` — Hybrid transactional/analytical processing (`pgbench-htap`: `pgbench_htap.pgbench.json`)
  - `workloads/pgbench-graph/` — Graph & social network analytics (`pgbench-graph`: `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json`)
  - `workloads/pgbench-vec/` — Multi-modal vector similarity search (`pgbench-vec`: `vec_hybrid.pgbench.json`, `vec_aging.pgbench.json`)
- `viewer/` (`src/bin/pgbench/viewer/`) — Standalone zero-dependency Python (`bottle.py`) + WebAssembly (`pgbench_chart.wasm`) web telemetry viewer (`pgbench_viewer.py`).
- `perffarm/` — Self-contained PostgreSQL Performance Farm ("perffarm") animal setup, hardware/disk discovery (`collect_instance_info.py`), client-server baseline `SELECT 1` & 1 MB throughput calibration (`calibrate_baseline.py`), Metrics DB schema (`sql/metrics_db_schema.sql`), and continuous benchmark runner (`run_perffarm.sh`) supporting local or remote/Cloud VM server and client topologies ([`perffarm/README.md`](perffarm/README.md)).
- `skills/` — Experimental Agentic skill ([`skills/pgbench4perffarm/SKILL.md`](skills/pgbench4perffarm/SKILL.md) and [`skills/README.md`](skills/README.md)) with progressive-disclosure references for AI coding and benchmarking agents.
- `AGENTS.md` — Top-level operational instructions, build/test commands, and non-negotiable guardrails for AI agents working in this repository ([`AGENTS.md`](AGENTS.md)).
- `doc/` — Comprehensive Markdown user guides ([`doc/README.md`](doc/README.md)), Agent-oriented guides ([`doc/agent-guide-pgbench.md`](doc/agent-guide-pgbench.md) and [`doc/agent-guide-perffarm.md`](doc/agent-guide-perffarm.md)), and DocBook SGML reference documentation (`doc/src/sgml/ref/pgbench.sgml` and `doc/src/sgml/pgbench-ext.sgml`).

## User Guides, Agentic Skill & Documentation (`doc/` & `skills/`)

- **[Documentation Index & Quick Reference](doc/README.md)** (`doc/README.md`)
- **[Experimental Agentic Skill (`pgbench4perffarm`)](skills/pgbench4perffarm/SKILL.md)** (`skills/pgbench4perffarm/SKILL.md` & [`skills/README.md`](skills/README.md)) — Progressive-disclosure skill for Gemini CLI, Claude Code, Cursor, Codex, and subagents covering both main `pgbench` usage and `perffarm/` orchestration.
- **[Agent-Oriented Guide: Main `pgbench` & `contrib/pgbench` Usage](doc/agent-guide-pgbench.md)** (`doc/agent-guide-pgbench.md`) — Intent-to-Workload dispatch matrix, initialization state machine, CLI mutual exclusions, exit code `0`/`1`/`2` triage tree, JSONPath assertions, and copy-pasteable subagent prompt templates.
- **[Agent-Oriented Guide: Perffarm Animal Setup, Execution & Regression Analysis](doc/agent-guide-perffarm.md)** (`doc/agent-guide-perffarm.md`) — Perffarm autonomous state machine, multi-duration (`--durations`) and multi-commit (`--commits-file`) sweep recipes, 5-point verification contract, cross-animal `remote_metrics_db` replication, and longitudinal SQL regression cookbook.
- **[General Usage, Features & Subsystems Overview](doc/user-guide.md)** (`doc/user-guide.md`) — Building & installing via `PGXS` (PostgreSQL 14–20devel), architectural overview of the 12 `pgbench` subsystems and `contrib/pgbench`, declarative `.pgbench.json` configuration (`--config`, `--dump-config`), heterogeneous client groups (`--group`), HDR latency histograms, SLA enforcement (`--sla`), JSON reporting (`--json-report`), `mmap` datasets, and trace replay.
- **[Database Metrics Streaming (`--log-db`) & Local WebAssembly Viewer Guide](doc/telemetry-and-viewer.md)** (`doc/telemetry-and-viewer.md`) — Streaming telemetry into the 3-tier, 9-table PostgreSQL schema (`--log-db`), in-database SQL inspection and regression comparison SRFs (`pgbench_show_run`, `pgbench_compare_runs`), and the zero-dependency `bottle.py` + `Water.css` + 439-byte WebAssembly Telemetry Viewer & SQL Data Explorer (`viewer/pgbench_viewer.py`).
- **[TPC-Inspired & HTAP Workloads Guide](doc/tpc-and-htap-workloads.md)** (`doc/tpc-and-htap-workloads.md`) — Parallel in-database initialization and execution guide for `pgbench-b`, `pgbench-c`, `pgbench-e`, `pgbench-h`, `pgbench-ds`, and CH-benCHmark-inspired `pgbench-htap`.
- **[Graph Database Workloads Guide](doc/graph-workloads.md)** (`doc/graph-workloads.md`) — LDBC-inspired social and financial graph benchmark suites (`LSQB`, `SNB Interactive v1/v2`, `SNB BI`, `FinBench`, and `Graphalytics`) across `SQL`, `SQL_UD` (`UNION DISTINCT ON`), and ISO `SQL/PGQ` (`GRAPH_TABLE`).
- **[Vector Similarity Search & Index Aging Guide](doc/vector-workloads.md)** (`doc/vector-workloads.md`) — VecBench-inspired vector ANN and hybrid RAG suite (`pgbench-vec` Modes 1–6), exact ground-truth generation, named `Recall@K` SLAs, 10-bucket tail distributions, and `Q1`-vs-`Q4` index aging degradation.
- **[Benchmark Scripting Language, Expressions & `contrib/pgbench` Guide](doc/benchmark-language-guide.md)** (`doc/benchmark-language-guide.md`) — Complete reference for the `PgBenchValue` runtime type system, all 39 backslash meta-commands, all 61 client-side expression functions, and all 51 `contrib/pgbench` server-side SQL functions.
- **[Perffarm Animal Setup & Continuous Benchmarking Guide](doc/perffarm-guide.md)** (`doc/perffarm-guide.md` & [`perffarm/README.md`](perffarm/README.md)) — Setting up a local or Cloud VM Perffarm animal (`setup_machine.sh`, `generate_pgconf.py`, `setup_metrics_db.sh`), recording per-instance CPU/RAM and separate system/data/WAL disk telemetry, calibrating client-server `SELECT 1` and 1 MB payload throughput, and running automated regression batches (`run_perffarm.sh`).

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

To run the `src/bin/pgbench` Perl TAP test suite (`t/001_pgbench_with_server.pl` and `t/002_pgbench_no_server.pl`):

```bash
make tapcheck PG_CONFIG=/path/to/pg_config
```

## Quick Start (`-s 10 -c 10 -T 10`)

```bash
# 1. Initialize a workload suite at scale factor 10 (run from src/bin/pgbench/ or $(pg_config --sharedir)/pgbench/)
pgbench -i -s 10 --config=workloads/pgbench-c/pgbench_c.pgbench.json <dbname>

# 2. Run a 10-second benchmark at concurrency 10
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json -s 10 -c 10 -j 2 -T 10 <dbname>
```

## License

Released under the [PostgreSQL License](COPYRIGHT).
