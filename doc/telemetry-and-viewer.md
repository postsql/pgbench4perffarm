# Database Metrics Streaming (`--log-db`) & Local WebAssembly Viewer Guide

Instead of writing flat log files to disk and parsing them with external scripts, `pgbench` can stream run metadata, summary statistics, HDR latency percentiles, `Recall@K` histograms, SLA evaluations, periodic `-P` progress snapshots, `--aggregate-interval` windows, and per-transaction `-l` traces directly into a PostgreSQL database via `--log-db`.

Alongside `--log-db`, `pgbench` installs SQL Set-Returning Functions (SRFs) for instant in-database run inspection and regression comparisons, and includes a zero-dependency web application (`viewer/pgbench_viewer.py`) powered by `bottle.py`, `Water.css`, and a 439-byte WebAssembly charting kernel (`wasm/pgbench_chart.wasm`).

---

## 1. Streaming Benchmark Telemetry to PostgreSQL (`--log-db`)

### 1.1 CLI Options for Database Telemetry

| CLI Option | Syntax / Values | Default | Description |
| :--- | :--- | :---: | :--- |
| `--log-db=<conninfo\|self>` | `self` or libpq conninfo / URI (`postgresql://...`) | *(disabled)* | Enables database telemetry logging. Pass `self` to log into the same database being benchmarked, or pass a separate connection string to isolate telemetry writes on a dedicated observer instance. |
| `--log-db-unlogged[=<mode>]` | `stream`, `all`, or `off` | `stream` | Controls table persistence (`LOGGED` vs. `UNLOGGED`):<br>• `stream`: Tier A summary tables are `LOGGED` (crash-safe); Tier B/C high-rate stream tables (`pgbench_progress_log`, `pgbench_agg_log`, `pgbench_tx_log`) are `UNLOGGED` for minimal WAL overhead.<br>• `all`: All 9 tables are created/altered to `UNLOGGED`.<br>• `off`: All 9 tables are `LOGGED`. |
| `--log-db-mode=<mode>` | `stream` or `deferred` | `stream` | Controls how per-thread `--aggregate-interval` and `-l` transaction samples are ingested:<br>• `stream`: Each worker thread opens a dedicated `COPY ... FROM STDIN` connection and flushes/commits batches every **64 KB** or **1 second** for real-time live viewing.<br>• `deferred`: Worker threads spool samples to local temporary files during the benchmark and bulk-load them via `COPY` after the benchmark timer stops, eliminating network/DB logging overhead during the run. |
| `--log-db-schema=<schema>` | PostgreSQL schema identifier | `public` | Target schema where `--log-db` tables, views, and SQL helper functions are provisioned (created automatically if it does not exist). |
| `--log-db-index` | *(no argument)* | `false` | Creates secondary B-tree indexes (`(run_id, elapsed_sec)`, `(run_id, interval_start_sec)`, `(run_id, ts_usec)`) on the Tier B/C stream tables after `COPY` completes. |
| `--run-label=<label>` | Arbitrary string | `NULL` | Attaches a human-readable label (e.g., `pg19-master`, `patch-v6-candidate`) to `pgbench_runs.run_label` and `--json-report` so runs can be queried and compared by label. |

### 1.2 End-to-End Streaming Examples

#### Example A: Log to the Target Database (`--log-db=self`)
```bash
pgbench --config=workloads/pgbench-c/pgbench_c.pgbench.json \
    -s 4 -c 8 -j 4 -T 30 -P 1 --aggregate-interval=1 -l -r \
    --log-db=self \
    --run-label="c-scale4-baseline" \
    --log-db-index \
    bench_c
```

#### Example B: Stream to a Dedicated Telemetry Database in a Custom Schema
```bash
pgbench --config=workloads/pgbench-htap/pgbench_htap.pgbench.json \
    -T 60 -P 1 --aggregate-interval=1 -l \
    --log-db="host=metrics-db.internal port=5432 dbname=perffarm" \
    --log-db-schema=pgbench_telemetry \
    --log-db-unlogged=stream \
    --log-db-mode=stream \
    --log-db-index \
    --run-label="htap-run-01" \
    bench_htap
```

#### Example C: Zero-Interference Deferred Mode for High-TPS Microbenchmarks
When benchmarking ultra-high-TPS workloads (e.g., `select-only` at 500k+ TPS with `-l` transaction logging enabled), use `--log-db-mode=deferred` so per-transaction records are spooled locally during the timed window and bulk-loaded via `COPY` at teardown:
```bash
pgbench -S -c 32 -j 8 -T 30 -P 1 -l \
    --log-db="dbname=perffarm" \
    --log-db-mode=deferred \
    --log-db-index \
    --run-label="select-only-deferred" \
    bench_b
```

> **Automatic Fallback Safety**: If the `--log-db` connection becomes unreachable at the end of a run, `pgbench` automatically writes the full JSON report to `pgbench_fallback_report_<run_id>.json` in the working directory so benchmark results are never lost.

---

## 2. The 3-Tier, 9-Table PostgreSQL Telemetry Schema

When `pgbench` starts with `--log-db`, `logDbBootstrapAndStartRun()` automatically provisions 9 tables, 2 views, and 6 SQL functions inside `--log-db-schema` (default `public`).

### 2.1 Tier A: Run Catalog & Summary Tables (`LOGGED` by default)

1. **`pgbench_runs`** *(Primary Key: `run_id bigserial`)*:
   - **Identity & Lifecycle**: `run_id`, `run_label`, `status` (`'running'`, `'completed'`, `'sla_failed'`, `'aborted'`), `exit_code`, `started_at`, `ended_at`, `host`, `target_db`.
   - **Workload Parameters**: `scale`, `clients`, `threads`, `duration_sec`.
   - **Counters & Throughput**: `transactions_cnt`, `transactions_failed`, `expected_rollbacks`, `transactions_skipped`, `retries`, `retried`, `tps`.
   - **HDR Latency (ms)**: `latency_avg_ms`, `latency_stddev_ms`, `latency_min_ms`, `latency_max_ms`, `latency_p50_ms`, `latency_p90_ms`, `latency_p95_ms`, `latency_p99_ms`, `latency_p99_9_ms`.
   - **Schedule Lag (ms)**: `lag_avg_ms`, `lag_p50_ms`, `lag_p90_ms`, `lag_p95_ms`, `lag_p99_ms`, `ontime_pct`.
   - **Vector Recall & Aging**: `recall_samples`, `recall_avg`, `recall_p5`, `recall_p50`, `recall_p95`, `recall_initial_q1`, `recall_mature_q4`, `recall_deg_pct`.
   - **Graph/Result Validation & SLAs**: `validation_passed`, `validation_failed`, `validation_pct`, `sla_all_passed`, `log_db_dropped_rows`.
   - **Full Documents**: `config jsonb` (effective `.pgbench.json` configuration) and `report jsonb` (full `--json-report` payload).
2. **`pgbench_run_groups`** *(Primary Key: `(run_id, group_idx)`)*:
   - Per-group breakdown for each `--group`: `group_name`, `clients`, `threads`, `sequential`, `target_rate`, `transactions_cnt`, `transactions_failed`, `expected_rollbacks`, `tps`, `latency_avg_ms`, `latency_p50_ms`..`latency_p99_9_ms`, `recall_samples`, `recall_avg`, `recall_p5`, `recall_p50`.
3. **`pgbench_run_scripts`** *(Primary Key: `(run_id, script_idx)`)*:
   - Per-script breakdown: `script_name`, `builtin_name`, `weight`, `transactions_cnt`, `transactions_failed`, `expected_rollbacks`, `tps`, `latency_avg_ms`, `latency_p50_ms`..`latency_p99_9_ms`, `recall_samples`, `recall_avg`, `recall_p5`, `recall_p50`.
4. **`pgbench_run_commands`** *(Primary Key: `(run_id, script_idx, cmd_idx)`)*:
   - Populated when `-r` (`--report-per-command`) is enabled: `cmd_type` (`sql` or `meta`), `first_line`, `cnt`, `failed`, `retries`, `latency_avg_ms`, `latency_stddev_ms`, `latency_min_ms`, `latency_max_ms`.
5. **`pgbench_run_recalls`** *(Primary Key: `(run_id, scope, metric_name)`)*:
   - Stores both overall and named recall streams (`r_unfiltered`, `r_prefilter`, `r_postfilter`, `r_multivec`, `r_aging`): `samples`, `avg_recall`, `stddev_recall`, `min_recall`, `max_recall`, `p5_recall`, `p50_recall`, `p95_recall`, `initial_q1`, `mature_q4`, `delta`, `degradation_pct`.
6. **`pgbench_run_slas`** *(Primary Key: `(run_id, sla_idx)`)*:
   - Stores every evaluated `--sla` rule: `raw_spec`, `target_scope`, `metric`, `op`, `threshold_val`, `actual_val`, `passed`.

### 2.2 Tier B & Tier C: High-Throughput Stream Tables (`UNLOGGED` by default)

7. **`pgbench_progress_log`** *(Tier B — populated when `-P <sec>` is enabled)*:
   - Columns: `run_id`, `ts_epoch_sec`, `elapsed_sec`, `tps`, `latency_avg_ms`, `latency_stddev_ms`, `lag_avg_ms`, `failed_cnt`.
   - Flushed and committed on every progress tick so live dashboards can plot real-time throughput and latency curves.
8. **`pgbench_agg_log`** *(Tier B — populated when `--aggregate-interval=<sec>` is enabled)*:
   - Columns: `run_id`, `thread_id`, `interval_start_sec`, `cnt`, `latency_sum_us`, `latency_sum2_us`, `latency_min_us`, `latency_max_us`, `lag_sum_us`, `lag_sum2_us`, `lag_min_us`, `lag_max_us`, `skipped_cnt`, `retried_cnt`, `retries_cnt`, `serialization_failures`, `deadlock_failures`, `other_sql_failures`, `recall_samples`, `recall_sum`, `validation_passed`, `validation_failed`.
9. **`pgbench_tx_log`** *(Tier C — populated when `-l` / `--log` is enabled)*:
   - Columns: `run_id`, `thread_id`, `client_id`, `group_id`, `script_idx`, `tx_cnt`, `ts_usec`, `status` (`'ok'`, `'skipped'`, `'failed'`), `latency_us`, `lag_us`, `retries`, `recall`, `validation_ok`.

### 2.3 Built-In Views

- **`pgbench_runs_v`**: Cleanly formatted catalog view of all runs ordered by `run_id DESC` with rounded numeric metrics (`tps`, `latency_avg_ms`, `latency_p50_ms`, `latency_p95_ms`, `latency_p99_ms`, `ontime_pct`, `recall_avg`, `recall_p5`, `recall_deg_pct`, `validation_pct`, `sla_all_passed`).
- **`pgbench_live_progress_v`**: Joins `pgbench_progress_log` with `pgbench_runs` (`run_id`, `run_label`, `status`, `target_db`, `progress_at`, `ts_epoch_sec`, `elapsed_sec`, `tps`, `latency_avg_ms`, `latency_stddev_ms`, `lag_avg_ms`, `failed_cnt`) ordered by `run_id DESC, elapsed_sec ASC` for real-time progress monitoring.

---

## 3. In-Database SQL Inspection & Regression Comparison SRFs

`pgbench` automatically installs 6 SQL functions (including 5 overloaded Set-Returning Functions and the `pgbench_viewer_payload_json` JSONB aggregator) in the telemetry schema so you can inspect runs and detect performance regressions directly from `psql` or CI scripts.

### 3.1 Single-Run Vertical Report (`pgbench_show_run`)

```sql
-- 1. Inspect the most recent benchmark run
SELECT * FROM pgbench_show_run();

-- 2. Inspect a specific run by numeric run_id
SELECT * FROM pgbench_show_run(42);

-- 3. Inspect the latest run matching a run_label
SELECT * FROM pgbench_show_run('c-scale4-baseline');
```

Returns rows of `(section text, scope text, metric text, value text, unit text, details text)` across 7 sections: `'1. Run Info'`, `'2. Overall Metrics'`, `'3. Client Groups'`, `'4. Scripts'`, `'5. Recall & Aging'`, `'6. Commands'`, and `'7. SLA Verifications'`.

### 3.2 Pairwise Regression Diff (`pgbench_compare_runs`)

Compare any two runs by `run_id` or `run_label`:

```sql
-- Compare baseline and candidate runs by run_label
SELECT scope, metric, unit,
       round(base_val::numeric, 3) AS base,
       round(cand_val::numeric, 3) AS cand,
       round(pct_change::numeric, 2) AS pct_change,
       verdict
FROM pgbench_compare_runs('pg20-baseline', 'pg20-candidate');

-- Or compare by numeric run_id
SELECT * FROM pgbench_compare_runs(1, 2);
```

Returns `(scope text, metric text, unit text, base_val float8, cand_val float8, abs_delta float8, pct_change float8, verdict text)` where `verdict` automatically classifies changes (`> 1%` threshold) using metric polarity awareness (`'IMPROVED'`, `'REGRESSED'`, `'UNCHANGED'`, or `'N/A'`):
- Higher-is-better metrics (`tps`, `ontime_pct`, `recall_avg`, `recall_p5`, `recall_p50`, `recall_p95`, `validation_pct`, `initial_q1`, `mature_q4`): positive delta (`> +1%`) → `IMPROVED`, negative delta (`< -1%`) → `REGRESSED`.
- Lower-is-better metrics (`latency_avg_ms`, `latency_p50_ms`..`latency_p99_9_ms`, `lag_avg_ms`, `lag_p95_ms`, `lag_p99_ms`, `transactions_failed`, `recall_deg_pct`, `degradation_pct`): negative delta (`< -1%`) → `IMPROVED`, positive delta (`> +1%`) → `REGRESSED`.

### 3.3 Multi-Run N-Way Matrix Comparison (`pgbench_compare_runs(bigint[])`)

Compare 2 or more runs simultaneously (e.g., across scale factors, PostgreSQL major versions, or patch iterations) relative to the first run in the array (`p_run_ids[1]`):

```sql
SELECT scope, metric, unit,
       run_id, run_label,
       round(val::numeric, 3) AS val,
       round(delta_vs_base::numeric, 3) AS delta_vs_base,
       round(pct_vs_base::numeric, 2) AS pct_vs_base,
       sla_all_passed
FROM pgbench_compare_runs(ARRAY[1, 2, 3, 4]::bigint[]);
```

---

## 4. Using the Local WebAssembly Telemetry Viewer (`viewer/pgbench_viewer.py`)

`src/bin/pgbench/viewer/pgbench_viewer.py` (also symlinked at `viewer/pgbench_viewer.py`) is a standalone, zero-dependency web application that connects to your `--log-db` PostgreSQL database and renders interactive dashboards, live monitors, and SQL explorations.

### 4.1 Starting the Viewer

```bash
python3 viewer/pgbench_viewer.py \
    --log-db="host=localhost port=5432 dbname=bench_c" \
    --schema=public \
    --host=0.0.0.0 \
    --port=8094
```

Then open `http://localhost:8094` in your browser.

#### CLI Flags & Environment Variables

| Flag | Environment Variable | Default | Description |
| :--- | :--- | :--- | :--- |
| `--log-db CONNINFO`, `--conninfo CONNINFO` | `PGBENCH_LOG_DB` | `host=localhost port=54320 dbname=pgbench_logdb_test` | PostgreSQL connection string (`libpq` key=value or `postgresql://` URI). |
| `--schema SCHEMA` | `PGBENCH_LOG_DB_SCHEMA` | `public` | Schema containing `pgbench_runs` and telemetry functions. |
| `--host HOST` | — | `0.0.0.0` | Bind address for the HTTP server. |
| `--port PORT` | — | `8094` | HTTP listen port. |
| `--psql-bin PATH` | — | `psql` | Fallback `psql` binary used automatically if Python `psycopg` (`psycopg3`) is not installed. |

> **Zero External Python Dependencies**: `pgbench_viewer.py` vendors `bottle.py` (`v0.12.20`) in the same directory and automatically falls back to invoking `psql -X -q -t -A` when `psycopg` is not installed in the Python environment.

### 4.2 Navigating the Five Viewer Modes

1. **Single-Run Breakdown (`GET /?mode=single&run_id=<id>`)**:
   - Displays top-level KPI summary cards (TPS, `p50`/`p95`/`p99` latency, transaction counts, `Recall@K`, validation rate, SLA pass/fail badge).
   - Renders a 3-chart WebAssembly SVG grid:
     - **TPS Over Time** (from `pgbench_progress_log` or `pgbench_agg_log`).
     - **Average Latency Over Time (ms)**.
     - **HDR Latency Percentile Profile (`min`, `p50`, `p90`, `p95`, `p99`, `p99.9`, `max`)**.
   - Displays the collapsible `pgbench_show_run(run_id)` vertical breakdown, per-group/per-script tables, SLA rule verifications, and per-command latency profile (`pgbench_run_commands`).
2. **Pairwise Run Comparison (`GET /?mode=compare&base_id=<b_id>&cand_id=<c_id>`)**:
   - Overlays baseline and candidate TPS time-series curves on a dual-series WASM SVG chart.
   - Renders the full `pgbench_compare_runs(base_id, cand_id)` diff table with color-coded `IMPROVED`, `REGRESSED`, and `UNCHANGED` badges.
3. **Multi-Run Matrix (`GET /?mode=multi&run_ids=1,2,3`)**:
   - Plots a multi-run WASM SVG bar chart comparing throughput and latency across all selected `run_ids`.
   - Renders the N-way `pgbench_compare_runs(bigint[])` matrix with `val`, `delta_vs_base`, `pct_vs_base` relative to the first selected run, and `sla_all_passed`.
4. **Real-Time Live Monitor (`GET /live`)**:
   - Automatically detects any benchmark currently executing with `status = 'running'` in `pgbench_runs`.
   - Polls `/api/payload?mode=live` every 1s, 2s, or 5s (configurable in the toolbar) and updates live KPI cards, cumulative TPS, and streaming WASM SVG progress charts in place without page reloads.
5. **Two-Stage Metatemplate SQL Data Explorer (`GET /explorer`)**:
   - Provides 6 curated one-click SQL presets (`1. Runs Overview (pgbench_runs_v)`, `2. Latest Run Full Breakdown (pgbench_show_run())`, `3. Top Slowest SQL/Meta Commands Across Runs`, `4. Per-Second Throughput & Tail Latency (pgbench_tx_log)`, `5. Vector Recall@K & Index Aging Degradation`, `6. Live Progress Stream (pgbench_live_progress_v)`) plus an interactive editor for custom read-only `SELECT` / `WITH` / `TABLE` / `VALUES` queries (up to `max_rows = 500` rows inside a `READ ONLY` transaction).
   - **Two-Stage Bottle Metatemplate Engine**:
     - Stage 1 inspects the returned JSONB columns and runtime value types (`run_link`, `badge`, `meter_pct`, `meter_ratio`, `metric_num`, `num`, `json`, `text`) and expands `viewer/html/explorer_table_meta.html.stpl` (using custom `<@ @> @ {| |}` delimiters) into a tailored Stage-2 `bottle.SimpleTemplate` (`stpl`) template.
     - Stage 2 renders the synthesized template into semantic HTML with inline percentage meters, clickable `run_id` links, status badges, and an automatic WASM SVG bar chart for numeric columns.
     - Expand `<details>Inspect Generated Stage-2 Bottle SimpleTemplate</details>` on the `/explorer` page to view the exact Stage-2 `.stpl` template generated for your query.

### 4.3 WebAssembly Charting Kernel (`wasm/pgbench_chart.wasm`)

All SVG line and bar charts in the viewer are projected client-side by a minimalistic **439-byte** WebAssembly module (`viewer/wasm/pgbench_chart.wasm`) compiled from `viewer/src/pgbench_chart.wat` (and reference C99 source `viewer/src/pgbench_chart.c`). It exports linear memory (`2` pages) and four IEEE-754 `f64` vector kernels:
- `series_min(ptr, n) -> f64`
- `series_max(ptr, n) -> f64`
- `series_mean(ptr, n) -> f64`
- `project_1d(in_ptr, n, min_v, max_v, out_start, out_span, out_ptr) -> void`

To rebuild or verify the byte-for-byte reproducibility and functional accuracy of `wasm/pgbench_chart.wasm` at any time:

```bash
python3 viewer/src/build_wasm.py --verify
```
