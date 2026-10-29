<!--
  INSPIRATION & TRADEMARK DISCLAIMER:
  The benchmark workloads in this repository are clean-room, non-comparable engineering workloads
  historically inspired by industry benchmarks (such as TPC-B, TPC-C, TPC-E, TPC-H, TPC-DS,
  CH-benCHmark, LDBC, and VecBench). Those names are mentioned ONLY in this opening header comment
  to acknowledge architectural inspiration. Throughout all file names, titles, documentation,
  configurations, and scripts, always use the clean-room `pgbench-*` workload names.
-->
# `pgbench4perffarm` User Guide: General Usage, Features & Subsystems Overview

`pgbench4perffarm` is a standalone, cross-version distribution of the next-generation PostgreSQL `pgbench` benchmarking system (extracted from patch series `v6`). It builds cleanly via `PGXS` against **PostgreSQL 14, 15, 16, 17, 18, 19, and 20devel**, providing:

1. **Modular `pgbench` Client Driver (`src/bin/pgbench/`)**: Multi-group client orchestration (`--group`), declarative JSON/JSONC configuration (`--config`, `--dump-config`), structured loops and exception handling (`\for`, `\while`, `\try`/`\catch`), typed runtime values (`TEXT`, `TIMESTAMP`, `ARRAY`, `VECTOR`, `BYTEA`), lock-free shared reservoirs (`--reservoir`), causal consistency watermarks, phased execution barriers (`\phase_barrier`), in-script result validation (`\validate_result`), ANN `Recall@K` & `nDCG@K` telemetry (`\record_recall`), base-2 HDR latency histograms, SLA enforcement (`--sla`), structured JSON reporting (`--json-report`), and direct PostgreSQL telemetry streaming (`--log-db`).
2. **Server-Side C Generator Extension (`contrib/pgbench/`)**: Exposes 51 SQL-callable C functions in `SCHEMA pgbench` (`CREATE EXTENSION pgbench;`) sharing the exact same deterministic PRNG, hashing, permutation, string, calendar, graph, and vector generation algorithms (`src/common/pgbench_funcs.c`) as the `pgbench` client.
3. **Eight Ready-to-Run Workload Suites (`workloads/`)**: Self-contained, zero-external-dependency benchmark suites covering OLTP (`pgbench-banking` / `pgbench-b`, `pgbench-order` / `pgbench-c`, `pgbench-brokerage` / `pgbench-e`), Decision Support OLAP (`pgbench-analytics` / `pgbench-h`, `pgbench-retail` / `pgbench-ds`), Hybrid HTAP (`pgbench-htap`), Graph Analytics across 4 query engines (`pgbench-graph`, `pgbench-graph-sql-ud`, `pgbench-graph-pgq`, `pgbench-graph-plpgq`), and Vector ANN Search & Index Aging (`pgbench-vec`).
4. **Local WebAssembly Telemetry Viewer (`viewer/`)**: A zero-dependency Python (`bottle.py`) + `Water.css` + 439-byte WebAssembly (`wasm/pgbench_chart.wasm`) web application for inspecting single runs, pairwise regression diffs, multi-run matrices, live streaming progress, and interactive SQL explorations.

> **Disclaimer**: All `pgbench-*` workload suites are **non-comparable, clean-room engineering workloads** for PostgreSQL engineering and testing. Results represent un-audited internal engineering measurements and do not constitute official certified industry benchmark results.

---

## 1. Building, Installing & Verifying

### 1.1 Prerequisites
- A PostgreSQL installation (**14 through 20devel**) with development headers (`pg_config` in `$PATH` or specified via `PG_CONFIG=/path/to/pg_config`).
- Standard C build toolchain (`gcc` or `clang`, `make`, `bison`, `flex`).
- Optional: Perl `IPC::Run` and `prove` for running the `src/bin/pgbench/t/*.pl` TAP test suites; Python 3 (`>= 3.8`) for running `viewer/pgbench_viewer.py`.

### 1.2 Compilation & Installation

From the repository root:

```bash
# Build both contrib/pgbench (server C extension) and src/bin/pgbench (client binary)
make -j"$(nproc)" PG_CONFIG=/path/to/pg_config

# Install pgbench.so + pgbench--1.0.sql into the server and pgbench + workloads/ + viewer/ into prefix
make install PG_CONFIG=/path/to/pg_config
```

What `make install` installs into your PostgreSQL prefix:
- `$(bindir)/pgbench` — The enhanced `pgbench` client binary.
- `$(pkglibdir)/pgbench.so` and `$(sharedir)/extension/pgbench--1.0.sql`, `pgbench.control` — The `contrib/pgbench` server extension.
- `$(sharedir)/pgbench/workloads/` — All workload suites and `.pgbench.json` configuration files.
- `$(sharedir)/pgbench/viewer/` — The standalone web telemetry viewer (`pgbench_viewer.py`, `bottle.py`, `html/`, `css/`, `wasm/`).

### 1.3 Running the Built-In Test Suites

```bash
# 1. Run contrib/pgbench SQL regression tests against a running PostgreSQL instance
make installcheck PG_CONFIG=/path/to/pg_config

# 2. Run src/bin/pgbench Perl TAP tests (001_pgbench_with_server.pl & 002_pgbench_no_server.pl)
make tapcheck PG_CONFIG=/path/to/pg_config
```

---

## 2. Subsystems & Architectural Overview

The `pgbench` codebase is modularized into 12 cohesive subsystems with clean ownership boundaries:

```
+-----------------------------------------------------------------------------------+
|                               pgbench.c / pgbench.h                               |
|                CLI Argument Parser, Global State & Main Orchestrator              |
+-----------------------------------------+-----------------------------------------+
                                          |
        +---------------------------------+---------------------------------+
        |                                 |                                 |
        v                                 v                                 v
+---------------+               +-------------------+               +---------------+
| config.c / .h |               |   engine.c / .h   |               |  init.c / .h  |
| --config JSON |               |  CState / TState  |               | -i & Multi-   |
| --dump-config |               |  Non-Blocking FSM |               | Stage Init    |
+-------+-------+               +---------+---------+               +---------------+
        |                                 |
        +----------------+----------------+----------------+----------------+
        |                |                |                |                |
        v                v                v                v                v
+---------------+ +--------------+ +--------------+ +--------------+ +--------------+
| script.c / .h | |commands.c/.h | |dataset.c / .h| | stats.c / .h | | logdb.c / .h |
| exprscan.l    | |39 Backslash  | |mmap Datasets | |HDR Histograms| |--log-db COPY |
| exprparse.y   | |Meta-Commands | |Reservoirs &  | |Recall@K Rings| |9 Tables &    |
| 61 Functions  | |Dispatch Table| |Watermarks    | |SLA Verifier  | |SQL Diff SRFs |
+-------+-------+ +------+-------+ +--------------+ +--------------+ +------+-------+
        |                |                                                  |
        v                v                                                  v
+---------------+ +--------------+                                  +---------------+
|variable.c / .h| | poller.c / .h|                                  |    viewer/    |
|Scoped Vars &  | |ppoll / select|                                  |bottle.py +    |
|PgBenchValue   | |Socket Mux    |                                  |439B WASM Chart|
+-------+-------+ +--------------+                                  +---------------+
        |
        v
+-----------------------------------------------------------------------------------+
|            src/common/pgbench_funcs.c & src/include/common/pgbench_funcs.h        |
|    Shared Deterministic PRNG, Hashes, Permutations, NURand, Vectors & Graphs      |
+-----------------------------------------+-----------------------------------------+
                                          |
                                          v
+-----------------------------------------------------------------------------------+
|                     contrib/pgbench (CREATE EXTENSION pgbench)                    |
|              51 Server-Side C SQL Functions Installed in SCHEMA pgbench           |
+-----------------------------------------------------------------------------------+
```

### Subsystem Directory & Responsibilities

| Subsystem | Source Files | Core Responsibilities |
| :--- | :--- | :--- |
| **CLI & Orchestration** | `src/bin/pgbench/pgbench.c`, `pgbench.h` | CLI option parsing, flag validation, client group setup (`parseClientGroup`), `--sla` rule parsing (`parseSlaSpec`), and top-level lifecycle management. |
| **Declarative Config** | `src/bin/pgbench/config.c`, `config.h` | Parses `.pgbench.json` (JSON and JSONC with `//`, `#`, `/*...*/` comments and trailing commas), resolves relative script/dataset paths, executes `init_steps[]` when `-i` is passed, replays `--json-report` files directly, and exports effective configurations via `--dump-config`. |
| **Execution Engine** | `src/bin/pgbench/engine.c`, `engine.h` | Multi-threaded worker loop (`threadRun`), non-blocking client state machine (`advanceConnectionState`), libpq pipeline execution, error/retry handling (`\try`/`\catch`, serialization `40001` & deadlock `40P01`), and `--json-report` generation. |
| **Script & Expression Compiler** | `src/bin/pgbench/script.c`, `script.h`, `exprscan.l`, `exprparse.y` | Parses custom and built-in scripts, statically links control-flow jump targets (`\if`, `\for`, `\while`, `\try`/`\catch`), compiles expressions via Flex/Bison, and evaluates all 61 client-side functions (`evaluateExpr`). |
| **Meta-Command Handlers** | `src/bin/pgbench/commands.c`, `commands.h` | Pluggable `CommandDescriptor` registry and runtime handlers for all 39 backslash meta-commands (`\set`, `\gset`, `\aset`, `\gset_col`, `\gset_meta`, `\sleep`, `\keying_time`, `\think_time`, `\call`, `\phase_barrier`, `\validate_result`, `\record_recall`, etc.). |
| **Scoped Variables & Types** | `src/bin/pgbench/variable.c`, `variable.h` | Scoped variable store (`VariableSpace` for subroutine `\call` frames) and tagged-union `PgBenchValue` supporting `INT`, `DOUBLE`, `BOOLEAN`, `TEXT`, `TIMESTAMP`, `ARRAY`, `VECTOR`, `BYTEA`, and `NULL`, with automatic SQL literal quoting and `:arr[:idx]` subscripting. |
| **Statistics & SLAs** | `src/bin/pgbench/stats.c`, `stats.h` | `SimpleStats` and `StatsData` accumulators, 3,968-bucket base-2 sub-bucketed HDR latency and schedule-lag histograms (~0.78% relative error), 10-bucket `Recall@K` distributions, 32-sample Q1/Q4 aging ring buffers, and `mergeStats()`. |
| **Datasets, Reservoirs & Watermarks** | `src/bin/pgbench/dataset.c`, `dataset.h` | Zero-copy `mmap` loader for `.csv`, `.tsv`, `.fvecs`, `.bvecs`, and `.ivecs` files; lock-free atomic shared key reservoirs (`PgBenchReservoir`); 64-channel causal consistency watermarks (`PgBenchCausalClock`); and cross-thread `\phase_barrier` synchronization. |
| **Socket Multiplexer** | `src/bin/pgbench/poller.c`, `poller.h` | Cross-platform non-blocking socket polling abstraction (`ppoll()` on POSIX, `select()` tracking both read and write fd sets on Windows). |
| **Database Initializer** | `src/bin/pgbench/init.c`, `init.h` | Standard `-i` (`tpcb-like`) table creation/population and multi-stage `--config=<file> -i` initialization step runner. |
| **Database Telemetry (`--log-db`)** | `src/bin/pgbench/logdb.c`, `logdb.h` | Bootstraps the 3-tier 9-table PostgreSQL telemetry schema, streams `-P` progress, `--aggregate-interval` windows, and `-l` transaction traces via `COPY ... FROM STDIN`, and installs SQL comparison SRFs (`pgbench_show_run`, `pgbench_compare_runs`). |
| **Shared PRNG & `contrib/pgbench`** | `src/common/pgbench_funcs.c`, `contrib/pgbench/` | Frontend/backend-agnostic C implementations of PRNG distributions, Murmur2/FNV-1a hashes, Feistel permutations (`permute`), `nurand`, `c_last`, date arithmetic, clustered/planted vectors, and R-MAT/community-correlated graph edges. |
| **Web Telemetry Viewer** | `src/bin/pgbench/viewer/` | Zero-dependency `bottle.py` + `Water.css` + 439-byte WebAssembly (`pgbench_chart.wasm`) web dashboard and two-stage metatemplate SQL Data Explorer. |

---

## 3. Declarative JSON/JSONC Configurations (`--config` & `--dump-config`)

Instead of passing long multi-line CLI commands, every workload suite provides ready-to-run `.pgbench.json` configuration files. The parser supports standard JSON as well as **JSONC** (`//` and `#` line comments, `/* ... */` block comments, and trailing commas).

### 3.1 Key Capabilities
1. **Relative Path Resolution**: All script and dataset file paths inside a `.pgbench.json` file are automatically resolved relative to the directory containing the `.pgbench.json` file, so you can invoke `pgbench --config=...` from any working directory.
2. **Multi-Stage Initialization (`-i`)**: When `-i` (`--initialize`) is combined with `--config=<file>`, `pgbench` executes the `init_steps` array defined in the JSON file in sequence (for example: `schema` → parallel `data` population → `constraints` → `ground_truth`).
3. **CLI Precedence**: Any flag passed on the command line (`-c`, `-j`, `-T`, `-t`, `-s`, `-D`, `--sla`, etc.) overrides the corresponding setting in `--config=<file>`, regardless of whether the CLI flag appears before or after `--config`.
4. **Direct Replay of `--json-report` Files**: Every JSON file produced by `--json-report=<file>` embeds a top-level `"configuration"` object. Passing `--config=<report.json>` reloads the exact scripts, weights, client groups, reservoirs, scale factor, and SLA rules from that prior run.
5. **Exporting Effective Config (`--dump-config`)**: Combine `--config` and CLI overrides with `--dump-config=<path>` (or `--dump-config=-` for `stdout`) to serialize the merged configuration as clean JSON and exit `0`.

### 3.2 Example `.pgbench.json` Structure

```jsonc
{
  // Suite metadata
  "suite": "pgbench-htap",
  "description": "Hybrid OLTP + OLAP workload with isolated client groups",
  "scale": 4,
  "no_vacuum": true,
  "duration_sec": 60,

  // Executed in order when running: pgbench --config=... -i <dbname>
  "init_steps": [
    { "name": "schema", "file": "pgbench_htap_schema.sql" },
    {
      "name": "data",
      "file": "pgbench_htap_init.sql",
      "clients": 4,
      "threads": 4,
      "variables": { "num_workers": 4 }
    },
    { "name": "constraints", "file": "pgbench_htap_constraints.sql" }
  ],

  // Independent client workload pools
  "groups": [
    {
      "name": "oltp",
      "clients": 8,
      "threads": 4,
      "scripts": [
        { "file": "pgbench_htap_oltp.sql", "weight": 1 }
      ]
    },
    {
      "name": "olap",
      "clients": 2,
      "threads": 2,
      "scripts": [
        { "file": "pgbench_htap_olap_q1.sql", "weight": 20 },
        { "file": "pgbench_htap_olap_q3.sql", "weight": 20 },
        { "file": "pgbench_htap_olap_q5.sql", "weight": 20 },
        { "file": "pgbench_htap_olap_q6.sql", "weight": 20 },
        { "file": "pgbench_htap_olap_q18.sql", "weight": 20 }
      ]
    }
  ],

  // Per-group and overall SLA compliance rules
  "sla": [
    "oltp:p95<=5000ms",
    "olap:p95<=10000ms"
  ]
}
```

---

## 4. Heterogeneous Client Groups (`--group`)

Real-world database workloads rarely consist of a single homogeneous client pool. The repeatable `--group` option lets you run multiple independent client pools concurrently within a single `pgbench` process:

```bash
pgbench -n -T 60 -s 2 -D scale=2 \
  --group="name=oltp,clients=8,threads=4,file=workloads/pgbench-htap/pgbench_htap_oltp.sql" \
  --group="name=olap,clients=2,threads=2,sequential=true,file=workloads/pgbench-htap/pgbench_htap_olap_q1.sql@20,file=workloads/pgbench-htap/pgbench_htap_olap_q3.sql@20" \
  --group="name=refresh,clients=1,threads=1,rate=2,file=workloads/pgbench-analytics/pgbench_analytics_refresh.sql" \
  bench_htap
```

### `--group` Sub-Options
- `name=<string>`: Unique group name (exposed to scripts as `:group_name`, and addressable in `--sla="<group_name>:..."`).
- `clients=<N>`: Number of concurrent database sessions in this group.
- `threads=<M>`: Number of worker threads assigned to drive this group's clients.
- `file=<path>[@<weight>]`: Adds a custom SQL script (with optional selection weight) to this group. Repeatable.
- `builtin=<name>[@<weight>]`: Adds a built-in script (`tpcb-like`, `simple-update`, `select-only`) to this group.
- `sequential=true|false`: When `true`, clients step through the group's scripts in deterministic permuted order (`pgbench_permute(seq_counter++, num_scripts, stream_id + 1)`) instead of weighted random sampling—ideal for multi-stream OLAP power/throughput runs (`pgbench-h`).
- `rate=<R>`: Throttles the group to an aggregate target rate of `<R>` transactions per second using Poisson arrival intervals, independent of other groups.

> **Important Invariant**: When `--group` is used, do **not** pass top-level `-c`, `-j`, or `-f` flags; total clients and threads are automatically computed as the sum of the per-group `clients` and `threads`.

---

## 5. HDR Latency Histograms, SLAs (`--sla`) & JSON Reports (`--json-report`)

### 5.1 Base-2 Sub-Bucketed HDR Histograms
Every thread, client group, and script maintains a zero-allocation, 3,968-bucket High-Dynamic-Range (HDR) latency histogram (`31` power-of-two octaves × `128` linear sub-buckets per octave) alongside a matching schedule-lag histogram.
- Recording a transaction latency takes `O(1)` CPU instructions via `pg_leftmost_one_pos64()`.
- Merging across threads (`mergeStats()`) is a simple array addition with zero lock contention.
- Percentile queries (`p50`, `p90`, `p95`, `p99`, `p99.9`, or any arbitrary `p<X>`) achieve ≤ 0.78% relative error across a range from microseconds to hours.

### 5.2 Service Level Agreement Enforcement (`--sla`)
Pass one or more `--sla` specifications on the command line (or in `.pgbench.json`) to turn `pgbench` into an automated CI/CD pass/fail gate:

```
--sla=[<target>:][<named_metric>:]<metric><operator><threshold>
```

| Field | Supported Values | Examples |
| :--- | :--- | :--- |
| `<target>` | `total` / `all` / `*` (default when omitted), `<group_name>`, `<script_name>` (or script basename without `.sql`) | `total:`, `oltp:`, `snb_readers:`, `pgbench_order_new_order:` |
| `<named_metric>` | Any named recall stream recorded via `\record_recall ... <metric_name>` (or `schedule_lag` when combined with `:p<X>`) | `r_unfiltered:`, `r_prefilter:`, `r_aging:`, `schedule_lag:` |
| `<metric>` | `p<X>` (e.g., `p50`, `p95`, `p99`, `p99.9`), `avg` / `latency_avg`, `schedule_lag:p<X>` / `schedule_lag_p<X>`, `ontime_pct` *(when `-R` / `rate=` or `--replay` is active)*, `recall`, `recall_p<X>` / `recall:p<X>`, `tps` / `qps`, `error_rate`, `validation_failures`, `validation_pct` | `p95<=50ms`, `schedule_lag:p99<=10ms`, `ontime_pct>=95%`, `recall>=0.90`, `r_unfiltered:recall>=0.95`, `r_unfiltered:recall:p5>=0.80`, `tps>=500`, `error_rate<=0%`, `validation_failures<=0` |
| `<operator>` | `<=`, `>=` | `<=`, `>=` |
| `<threshold>` | Number with optional unit (`us`, `ms`, `s`, `%`) | `100ms`, `1.5s`, `0.95`, `99.9%`, `0` |

### 5.3 Process Exit Codes

| Exit Code | Meaning |
| :---: | :--- |
| **`0`** | Benchmark completed normally **and** all `--sla` compliance checks passed. |
| **`1`** | Either one or more `--sla` checks failed (`$.sla_all_passed == false` in `--json-report`), **or** a fatal CLI/connection error occurred prior to execution. |
| **`2`** | Benchmark was aborted mid-run (e.g., an unhandled SQL error outside `\try`/`\catch`, a failed expression evaluation, or a barrier abort). |

### 5.4 Structured JSON Reports (`--json-report=<file>`)
Passing `--json-report=results.json` writes a machine-readable JSON document containing:
- `"configuration"`: Complete effective benchmark configuration (directly replayable via `pgbench --config=results.json`).
- `"overall"`: Aggregate throughput (`tps`), transaction counts (`transactions_processed`, `transactions_failed`, `expected_rollbacks`, `transactions_skipped`), validation counters (`validation_passed`, `validation_failed`, `validation_pct`), HDR latency percentiles (`latency_ms`), schedule lag percentiles (`schedule_lag_ms`, `ontime_pct`), global `recall` (with 10-bucket distribution and Q1-vs-Q4 `aging_degradation`), and per-stream `named_recall`.
- `"groups"`: Per-group throughput, HDR latency percentiles, and recall metrics.
- `"scripts"`: Per-script throughput, weights, HDR latency percentiles, and per-command profiles (when `-r` is enabled).
- `"sla_verifications"` and `"sla_all_passed"`: Detailed breakdown of every `--sla` rule, threshold, actual measured value, and boolean verdict.

---

## 6. External Datasets (`--dataset`, `--vector-dataset`) & Trace Replay (`--replay`)

### 6.1 Zero-Copy `mmap` Datasets
In addition to in-database synthetic data generation via `contrib/pgbench`, `pgbench` can memory-map external columnar CSV/TSV files and binary vector files (`.fvecs`, `.bvecs`, `.ivecs`) into shared read-only memory (`--dataset="<name>[:<format>][:header]=<path>"` or `--dataset="<name>:<path>[:<format>][:header]"`):

```bash
# Memory-map a CSV dataset with a header row and bulk-load it into a table before running scripts
pgbench --dataset="users:/data/users.csv:csv:header" \
        --copy-dataset="users:app_users(id,name,email)" \
        -f workload.sql bench_db

# Memory-map SIFT1M / GIST1M binary vector files (.fvecs = float32, .ivecs = int32 ground truth)
pgbench --vector-dataset="base:/data/sift_base.fvecs" \
        --vector-dataset="queries:/data/sift_query.fvecs" \
        --vector-dataset="gt:/data/sift_groundtruth.ivecs" \
        -f vec_bench.sql bench_db

# Or use shorthand flags (registers datasets "base", "query", and "ground_truth" with optional 0based/1based ID offset)
pgbench --vector-base="pgbench_vec_items:0based:/data/sift_base.fvecs" \
        --vector-query="0based:/data/sift_query.fvecs" \
        --ground-truth-table="0based:/data/sift_groundtruth.ivecs" \
        -f vec_bench.sql bench_db
```

Inside scripts, bind rows using `\dataset_bind <name> <wrap|once|partitioned|sample|AT <idx>> var1 ...` or expression functions `dataset_count('name')`, `dataset_get('name', row, col)`, and `dataset_next_id('name', 'wrap')`.

### 6.2 Deterministic Trace Replay (`--replay`, `--replay-speed`, `--tcr`)
Replay production or benchmark timestamp traces directly with `--replay=<trace.csv>` or pace script execution using `\wait_until :ts_us`:
- `--replay-speed=<factor>`: Speeds up or slows down trace timestamps (`scaled_us = offset_us / replay_speed`; e.g., `2.0` runs 2× faster than recorded wall-clock intervals, and `0.5` runs at half speed).
- `--tcr=<factor>`: Alias for `--replay-speed=<factor>`.
- `--ontime-threshold=<ms>`: Defines the maximum schedule lag (default `50.0` ms) counted as "on-time" in `ontime_pct` (`--sla="ontime_pct>=95%"`).

---

## 7. Complete Summary of Workload Suites

| Suite | Directory | Domain | Key Features Exercised | Detailed Guide |
| :--- | :--- | :--- | :--- | :--- |
| **`pgbench-banking`** (`pgbench-b`) | `workloads/pgbench-banking/` (`pgbench-b/`) | Banking OLTP | Server-side stored procedures (`CALL`), `pgbench.random()` | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-order`** (`pgbench-c`) | `workloads/pgbench-order/` (`pgbench-order/`) | Order-Entry OLTP | 9 tables, 5 weighted scripts, `nurand`, `c_last`, `\try`/`\catch` 1% rollback, `\keying_time`/`\think_time`, `\for` | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-brokerage`** (`pgbench-e`) | `workloads/pgbench-brokerage/` (`pgbench-e/`) | Brokerage OLTP | 16 tables, 10 stored procedures, 6 weighted scripts, limit-order rollbacks | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-analytics`** (`pgbench-h`) | `workloads/pgbench-analytics/` (`pgbench-h/`) | Decision Support OLAP | 8 tables, `Q1`–`Q19` `sequential=true` stream + throttled `RF1`/`RF2` refresh `--group` | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-retail`** (`pgbench-ds`) | `workloads/pgbench-retail/` (`pgbench-ds/`) | Retail Star-Schema OLAP | 13 star-schema tables, CTE/window reporting `--group` + concurrent maintenance `--group` | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-htap`** | `workloads/pgbench-htap/` | Hybrid OLTP + OLAP | Operational + analytical schema, simultaneous `oltp` & `olap` groups with independent SLAs | [`relational-and-htap-workloads.md`](relational-and-htap-workloads.md) |
| **`pgbench-graph`** | `workloads/pgbench-graph*` | Social & Financial Graph | LSQB, SNB Interactive, SNB BI, FinBench, Graphalytics across `SQL`, `SQL_UD`, `PGQ`, and `plPGQ`; reservoirs, watermarks, `\phase_barrier`, `\validate_result` | [`graph-workloads.md`](graph-workloads.md) |
| **`pgbench-vec`** | `workloads/pgbench-vec/` | Vector ANN & Hybrid RAG | Modes 1–6 (unfiltered, pre-filtered, post-filtered, insert, multi-vec, index aging), exact ground truth, `\record_recall`, Q1-vs-Q4 degradation | [`vector-workloads.md`](vector-workloads.md) |
