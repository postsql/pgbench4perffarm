# `pgbench4perffarm` User Guides & Documentation

Welcome to the documentation for **`pgbench4perffarm`**, the standalone next-generation PostgreSQL `pgbench` client benchmark driver, `contrib/pgbench` server-side C extension, multi-domain workload suites (`workloads/pgbench-*`), direct PostgreSQL telemetry logger (`--log-db`), and WebAssembly telemetry viewer (`viewer/`).

> **Disclaimer**: All `pgbench-*` workload suites (`pgbench-b`, `pgbench-c`, `pgbench-e`, `pgbench-h`, `pgbench-ds`, `pgbench-htap`, `pgbench-graph`, `pgbench-vec`) are **non-comparable, inspired-by engineering workloads** for PostgreSQL testing and development. Results represent un-audited internal engineering measurements and do not constitute official TPC, LDBC, or VecBench benchmark results, nor are they comparable to any official published benchmark ratings.

---

## Documentation Catalog

| Guide | File | Topics Covered |
| :--- | :--- | :--- |
| **1. General Usage & Subsystems Overview** | [`user-guide.md`](user-guide.md) | Building & installing via `PGXS` (PostgreSQL 14–20devel), architectural overview of the 12 `pgbench` subsystems + `contrib/pgbench`, declarative `.pgbench.json` configs (`--config`, `--dump-config`), multi-group orchestration (`--group`), HDR latency histograms, SLA enforcement (`--sla`), JSON reports (`--json-report`), external `mmap` datasets, trace replay, and exit codes. |
| **2. Database Metrics Streaming & Local Viewer** | [`telemetry-and-viewer.md`](telemetry-and-viewer.md) | Streaming benchmark telemetry directly into PostgreSQL (`--log-db`, `--log-db-unlogged`, `--log-db-mode`, `--run-label`), the 3-tier 9-table telemetry schema, SQL inspection and regression diff SRFs (`pgbench_show_run`, `pgbench_compare_runs`), and using the zero-dependency `bottle.py` + `Water.css` + 439-byte WebAssembly Telemetry Viewer & SQL Data Explorer (`pgbench_viewer.py`). |
| **3. TPC-Inspired & HTAP Workloads Guide** | [`tpc-and-htap-workloads.md`](tpc-and-htap-workloads.md) | In-database parallel initialization and execution guide for relational OLTP, DSS/OLAP, and HTAP suites: **`pgbench-b`** (stored-proc banking), **`pgbench-c`** (order-entry OLTP), **`pgbench-e`** (brokerage OLTP), **`pgbench-h`** (decision support + refresh stream), **`pgbench-ds`** (star-schema retail analytics + maintenance), and **`pgbench-htap`** (CH-benCHmark-inspired hybrid OLTP + OLAP). |
| **4. Graph Database Workloads Guide** | [`graph-workloads.md`](graph-workloads.md) | LDBC-inspired graph benchmark suites across **4 execution models** (`SQL`, `SQL_UD` with `UNION DISTINCT ON`, native ISO `SQL/PGQ` `GRAPH_TABLE`, and `plPGQ`) and **4 workload families**: **LSQB** (`Q1`–`Q9`), **SNB Interactive v1/v2** (readers/writers, reservoirs, causal watermarks, `\call`), **SNB BI** (`\phase_barrier`), and **FinBench & Graphalytics** (`BFS`, `WCC`, `SSSP`, `PageRank`, `LCC`). |
| **5. Vector Similarity Search & Aging Guide** | [`vector-workloads.md`](vector-workloads.md) | VecBench-inspired vector ANN and hybrid RAG benchmark suite (**`pgbench-vec`**) covering exact ground-truth generation (`pgbench_vec_gt_gen.sql`), **Modes 1–5** (unfiltered, pre-filtered, post-filtered oversampling, streaming upsert, multi-vector fusion), **Mode 6** (continuous ingestion, drift, churn, and Q1-vs-Q4 index aging degradation), external `.fvecs`/`.bvecs`/`.ivecs` datasets, and named `Recall@K` SLAs. |
| **6. Benchmark Scripting Language Guide** | [`benchmark-language-guide.md`](benchmark-language-guide.md) | Complete reference for the `pgbench` scripting language: typed `PgBenchValue` variables (`INT`, `DOUBLE`, `BOOL`, `TEXT`, `TIMESTAMP`, `ARRAY`, `VECTOR`, `BYTEA`), all **39 backslash meta-commands** (`\for`, `\while`, `\try`/`\catch`, `\gset_col`, `\gset_meta`, `\phase_barrier`, `\reservoir_*`, `\validate_result`, `\record_recall`, etc.), all **61 client expression functions**, and all **51 `contrib/pgbench` server SQL functions**. |
| **7. Perffarm Animal Setup & Runner Guide** | [`perffarm-guide.md`](perffarm-guide.md) | One-time animal setup (`setup_machine.sh`, `generate_pgconf.py`, `setup_metrics_db.sh`), local vs. external/Cloud VM topologies (`local`, `remote-server`, `remote-client`, `both-remote`), per-instance CPU/RAM & separate system/data/WAL disk telemetry, client-server `SELECT 1` and 1 MB throughput calibration, Metrics DB schema (`perffarm_animals`, `perffarm_batches`, `perffarm_test_runs`, `perffarm_commit_summary_v`), and regular runs (`run_perffarm.sh`). |
| **8. SGML Reference Pages** | [`src/sgml/`](src/sgml/) | Upstream DocBook SGML reference sources for [`pgbench.sgml`](src/sgml/ref/pgbench.sgml) and [`pgbench-ext.sgml`](src/sgml/pgbench-ext.sgml). |

---

## Quick Start Overview

### 1. Build & Install (PostgreSQL 14 through 20devel)

```bash
# Build and install both contrib/pgbench (server extension) and pgbench (client driver)
make -j"$(nproc)" PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config

# Verify server extension SQL regression suite and client Perl TAP tests
make installcheck PG_CONFIG=/path/to/pg_config
make tapcheck PG_CONFIG=/path/to/pg_config
```

### 2. Initialize & Run Any Suite with a Single `--config` Command

```bash
# 1. Create a target database
createdb bench_c

# 2. Run the multi-stage initialization pipeline (schema -> parallel data -> constraints)
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json -i -s 4 bench_c

# 3. Run the workload for 30 seconds, enforce SLAs, stream metrics to PostgreSQL, and export JSON report
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json \
    -c 8 -j 4 -T 30 -P 1 -l \
    --log-db=self --run-label="c-baseline" --log-db-index \
    --json-report=report_c.json \
    bench_c
```

### 3. Inspect Results in SQL or Launch the Local Web Viewer

```bash
# Query the run summary directly in PostgreSQL
psql -d bench_c -c "SELECT * FROM pgbench_show_run('c-baseline');"

# Start the zero-dependency WebAssembly Telemetry Viewer & SQL Data Explorer
python3 viewer/pgbench_viewer.py --log-db="dbname=bench_c" --port=8094
```
