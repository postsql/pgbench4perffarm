# Main `pgbench` Usage, Fast Verification Plan & Agent Execution Recipes

This reference provides deterministic command recipes, initialization state machines, template database cloning patterns, JSONPath post-flight assertions, and error triage tables for running **`pgbench`** and **`contrib/pgbench`** in `postsql/pgbench4perffarm`.

---

## 1. Template Database Cloning Pattern (Skip Re-Initialization Across Sweep Runs)

Initializing multi-table analytical or graph workloads takes 5–60 seconds depending on scale factor. When comparing PostgreSQL GUC tuning, index definitions, or code patches across multiple runs, initialize a **golden template database once** and clone it in `< 0.5s` via `CREATE DATABASE ... TEMPLATE`:

```bash
PG_BIN="$(/path/to/pg_config --bindir)"
WORKLOADS="/path/to/pgbench4perffarm/src/bin/pgbench/workloads"

# 1. Create and initialize golden template database once
"$PG_BIN/createdb" bench_c_tpl
"$PG_BIN/pgbench" --config="$WORKLOADS/pgbench-c/pgbench_c.pgbench.json" -i -s 2 bench_c_tpl

# 2. For each experiment run, clone in <0.5s via filesystem copy
"$PG_BIN/dropdb" --if-exists bench_c_run
"$PG_BIN/createdb" -T bench_c_tpl bench_c_run

# 3. Execute timed benchmark against the fresh clone
"$PG_BIN/pgbench" --config="$WORKLOADS/pgbench-c/pgbench_c.pgbench.json" -s 2 -c 8 -j 4 -T 30 \
  --json-report=/tmp/bench_c_run.json bench_c_run
```

---

## 2. Workload-by-Workload Execution Recipes (`--config` & Manual CLI)

All workload manifests live under `src/bin/pgbench/workloads/` (or `$(pg_config --sharedir)/pgbench/workloads/` after `make install`).

### 2.1 `pgbench-b` (TPC-B-Inspired Stored-Procedure Banking OLTP)
```bash
W="src/bin/pgbench/workloads"
pgbench --config="$W/pgbench-b/pgbench_b.pgbench.json" -i -s 10 "$DB"
pgbench --config="$W/pgbench-b/pgbench_b.pgbench.json" -s 10 -c 8 -j 4 -T 30 \
  --sla="p95<=25ms" --sla="error_rate<=0%" --json-report=/tmp/pgbench_b.json "$DB"
```

### 2.2 `pgbench-c` (TPC-C-Inspired 9-Table Wholesale OLTP)
```bash
W="src/bin/pgbench/workloads"
# Single-command --config initialization & run
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -i -s 2 "$DB"
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -s 2 -c 8 -j 4 -T 30 \
  --json-report=/tmp/pgbench_c.json "$DB"

# Manual 3-stage initialization + weighted 5-script mix (45:43:4:4:4)
pgbench -n -t 1 -f "$W/pgbench-c/pgbench_c_schema.sql" "$DB"
pgbench -n -t 1 -c 4 -j 4 -D scale=2 -D num_workers=4 -f "$W/pgbench-c/pgbench_c_init.sql" "$DB"
pgbench -n -t 1 -f "$W/pgbench-c/pgbench_c_constraints.sql" "$DB"
pgbench -n -s 2 -D scale=2 -c 8 -j 4 -T 30 \
  -f "$W/pgbench-c/pgbench_c_new_order.sql@45" \
  -f "$W/pgbench-c/pgbench_c_payment.sql@43" \
  -f "$W/pgbench-c/pgbench_c_order_status.sql@4" \
  -f "$W/pgbench-c/pgbench_c_delivery.sql@4" \
  -f "$W/pgbench-c/pgbench_c_stock_level.sql@4" \
  --json-report=/tmp/pgbench_c.json "$DB"
```

### 2.3 `pgbench-e` (TPC-E-Inspired 16-Table Brokerage OLTP)
```bash
W="src/bin/pgbench/workloads"
pgbench --config="$W/pgbench-e/pgbench_e.pgbench.json" -i -s 1 "$DB"
pgbench --config="$W/pgbench-e/pgbench_e.pgbench.json" -s 1 -c 8 -j 4 -T 30 \
  --json-report=/tmp/pgbench_e.json "$DB"
```

### 2.4 `pgbench-h` (TPC-H-Inspired 8-Table Decision Support OLAP + Throttled Refresh)
```bash
W="src/bin/pgbench/workloads"
pgbench --config="$W/pgbench-h/pgbench_h.pgbench.json" -i -s 1 "$DB"
pgbench --config="$W/pgbench-h/pgbench_h.pgbench.json" -s 1 -T 60 \
  --json-report=/tmp/pgbench_h.json "$DB"
```

### 2.5 `pgbench-ds` (TPC-DS-Inspired 13-Table Star Schema OLAP + Maintenance)
```bash
W="src/bin/pgbench/workloads"
pgbench --config="$W/pgbench-ds/pgbench_ds.pgbench.json" -i -s 1 "$DB"
pgbench --config="$W/pgbench-ds/pgbench_ds.pgbench.json" -s 1 -T 60 \
  --json-report=/tmp/pgbench_ds.json "$DB"
```

### 2.6 `pgbench-htap` (CH-benCHmark-Inspired Hybrid OLTP + OLAP with Isolated Groups)
```bash
W="src/bin/pgbench/workloads"
# Single-command --config initialization & run
pgbench --config="$W/pgbench-htap/pgbench_htap.pgbench.json" -i -s 2 "$DB"
pgbench --config="$W/pgbench-htap/pgbench_htap.pgbench.json" -s 2 -T 60 \
  --json-report=/tmp/pgbench_htap.json "$DB"

# Explicit multi-group CLI with per-group SLAs
pgbench -n -s 2 -D scale=2 -T 60 \
  --group="name=oltp,clients=8,threads=4,file=$W/pgbench-htap/pgbench_htap_oltp.sql@1" \
  --group="name=olap,clients=2,threads=2,sequential=true,file=$W/pgbench-htap/pgbench_htap_olap_q1.sql@20,file=$W/pgbench-htap/pgbench_htap_olap_q3.sql@20,file=$W/pgbench-htap/pgbench_htap_olap_q5.sql@20,file=$W/pgbench-htap/pgbench_htap_olap_q6.sql@20,file=$W/pgbench-htap/pgbench_htap_olap_q18.sql@20" \
  --sla="oltp:p95<=5000ms" --sla="olap:p95<=10000ms" --sla="error_rate<=0%" \
  --json-report=/tmp/pgbench_htap.json "$DB"
```

### 2.7 `pgbench-graph` (LDBC-Inspired Graph Across `SQL`, `SQL_UD`, and `SQL/PGQ`)
Three workload directories cover the graph query engines, each containing 5 `.pgbench.json` manifests (`lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json`):
- `workloads/pgbench-graph/` — Standard `WITH RECURSIVE` SQL CTEs (PostgreSQL 14–20devel)
- `workloads/pgbench-graph-sql-ud/` — Recursive CTEs with `UNION DISTINCT ON (id ORDER BY weight)` (PostgreSQL 19+ / patched servers)
- `workloads/pgbench-graph-pgq/` — ISO `SQL/PGQ` `GRAPH_TABLE` over `CREATE PROPERTY GRAPH` (PostgreSQL 19+)

```bash
W="src/bin/pgbench/workloads"
# Initialize graph schema + correlated LDBC social network once
pgbench --config="$W/pgbench-graph/lsqb.pgbench.json" -i -s 1 "$DB"

# Run LSQB, SNB Interactive, and phased SNB BI
pgbench --config="$W/pgbench-graph/lsqb.pgbench.json" -s 1 -t 5 --json-report=/tmp/graph_lsqb.json "$DB"
pgbench --config="$W/pgbench-graph/snb_interactive.pgbench.json" -s 1 -T 10 --json-report=/tmp/graph_snb_ic.json "$DB"
pgbench --config="$W/pgbench-graph/snb_bi.pgbench.json" -s 1 -c 2 -j 2 -t 2 --json-report=/tmp/graph_snb_bi.json "$DB"
```
> **CRITICAL**: Any script containing `\phase_barrier` (`snb_bi.pgbench.json`) **must** be executed with bounded transaction count `-t <N>`, never `-T <seconds>`.

### 2.8 `pgbench-vec` (VecBench-Inspired Hybrid Vector ANN Search & Index Aging Degradation)
```bash
W="src/bin/pgbench/workloads"
# Initialize vector schema + clustered embeddings + exact ground truth (pgbench_vec_gt_gen.sql)
pgbench --config="$W/pgbench-vec/vec_hybrid.pgbench.json" -i -s 1 "$DB"

# Run Modes 1–5 hybrid search (unfiltered, pre-filter, post-filter, insert, multi-vec) with named Recall@K SLAs
pgbench --config="$W/pgbench-vec/vec_hybrid.pgbench.json" -s 1 -t 20 --json-report=/tmp/vec_hybrid.json "$DB"

# Run Mode 6 continuous churn & Q1-vs-Q4 index aging degradation
pgbench --config="$W/pgbench-vec/vec_aging.pgbench.json" -s 1 -t 20 --json-report=/tmp/vec_aging.json "$DB"
```

---

## 3. Fast End-to-End Verification Script (`<= 90 Seconds` for All Suites)

Run this self-contained bash script to verify `pgbench`, `contrib/pgbench`, `--config`, `--group`, `--reservoir`, `--sla`, `--json-report`, and `--log-db` across the core suites in under 90 seconds:

```bash
#!/usr/bin/env bash
set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
PG_BIN="$("$PG_CONFIG" --bindir)"
REPO_ROOT="${REPO_ROOT:-$(pwd)}"
WORKLOADS="$REPO_ROOT/src/bin/pgbench/workloads"
export PGPORT="${PGPORT:-54320}"
export PGHOST="${PGHOST:-/tmp}"

"$PG_BIN/dropdb" --if-exists pgbench_verify_db pgbench_verify_metrics
"$PG_BIN/createdb" pgbench_verify_db
"$PG_BIN/createdb" pgbench_verify_metrics

declare -A MANIFESTS=(
  ["pgbench-b"]="pgbench-b/pgbench_b.pgbench.json"
  ["pgbench-c"]="pgbench-c/pgbench_c.pgbench.json"
  ["pgbench-e"]="pgbench-e/pgbench_e.pgbench.json"
  ["pgbench-h"]="pgbench-h/pgbench_h.pgbench.json"
  ["pgbench-ds"]="pgbench-ds/pgbench_ds.pgbench.json"
  ["pgbench-htap"]="pgbench-htap/pgbench_htap.pgbench.json"
  ["pgbench-graph"]="pgbench-graph/lsqb.pgbench.json"
  ["pgbench-vec"]="pgbench-vec/vec_hybrid.pgbench.json"
)

for suite in pgbench-b pgbench-c pgbench-e pgbench-h pgbench-ds pgbench-htap pgbench-graph pgbench-vec; do
  cfg="$WORKLOADS/${MANIFESTS[$suite]}"
  echo "=== Verifying $suite ($cfg) ==="
  "$PG_BIN/dropdb" --if-exists pgbench_verify_db
  "$PG_BIN/createdb" pgbench_verify_db
  "$PG_BIN/pgbench" --config="$cfg" -i -s 1 pgbench_verify_db
  "$PG_BIN/pgbench" --config="$cfg" -s 1 -t 3 \
    --json-report="/tmp/${suite}.json" \
    --log-db="dbname=pgbench_verify_metrics" \
    --run-label="$suite" pgbench_verify_db || true
  python3 -c "import json; d=json.load(open('/tmp/${suite}.json')); assert d['overall']['transactions_processed'] > 0, '$suite had 0 tx'"
done
echo "ALL SUITES VERIFIED SUCCESSFULLY"
```

---

## 4. Post-Flight `--json-report` JSONPath Assertions

Never rely solely on stdout parsing when `--json-report=<file>` is available. Validate these JSON paths after every run:

| JSON Path | Expected Condition | Meaning |
| :--- | :--- | :--- |
| `$.configuration` | object present | Complete effective benchmark configuration (directly replayable via `pgbench --config=<report.json>`) |
| `$.overall.transactions_processed` | `> 0` | Non-zero transactions completed |
| `$.overall.transactions_failed` | `== 0` | Zero unexpected failed transactions (expected rollbacks inside `\try`/`\catch` increment `$.overall.expected_rollbacks`) |
| `$.overall.tps` | `> 0.0` | Overall throughput |
| `$.overall.latency_ms.p95` | `> 0.0` | HDR 95th percentile latency in ms |
| `$.overall.validation_failed` | `== 0` | Zero `\validate_result` invariant failures (`$.overall.validation_pct == 100.0`) |
| `$.overall.recall.avg_recall` | `>= 0.80` | Acceptable ANN Recall@K vs. exact ground truth (`pgbench-vec`) |
| `$.sla_all_passed` | `== true` | Every `--sla` rule passed (`false` when exit code `1` is due to SLA breach) |
| `$.sla_verifications[*].passed` | `== true` | Per-SLA rule evaluation detail |
| `$.groups[*].tps` | `> 0.0` | Per-group throughput |

---

## 5. Deterministic Error Triage Table (`pgbench`)

| Symptom / Error Message | Root Cause | Deterministic Fix |
| :--- | :--- | :--- |
| `pgbench: error: some of the specified options cannot be used in initialization (-i) mode` | Passed `-i` alongside `-f *_schema.sql` (or other benchmarking flags) | Remove `-i` when using `-f`. Use `-n -t 1 -f ...` for manual SQL init, or `--config=<file>.pgbench.json -i` without `-f`. |
| `--config` groups ignored when `-f`, `-b`, or `--group` is passed on CLI | Passing `-f`, `-b`, or `--group` on CLI invokes `clearConfigScriptsAndGroups()` | Either use `--config=<file>.pgbench.json [-c N -j M]` (which proportionally scales configured groups), or define all groups via `--group="name=...,clients=N,threads=M,file=path@weight"`. |
| `invalid command in command "reservoir_pop"` (or `"break"` / `"continue"`) | Used a non-existent backslash meta-command | Use `\reservoir_sample <reservoir_name> <var_name> [<default_expr>]` for reservoir sampling; use `\if` / `\while` conditions instead of `\break` / `\continue`. |
| `missing variable name argument in command "gset_col"` | Invoked `\gset_col` without a target variable name | Pass `\gset_col <var_name> [<col_name_or_idx>]` (or use `\aset <prefix>` to bind all columns across all rows into `<prefix><col>` arrays). |
| `function pgbench.nurand(...) does not exist` | `contrib/pgbench` extension not installed in target DB | Run `make install` and `CREATE EXTENSION IF NOT EXISTS pgbench;` in target DB (or use `--config=... -i` whose schema script creates it). |
| `syntax error at or near "PROPERTY"` | Ran `pgbench-graph-pgq/` on PG < 19 | Switch to `workloads/pgbench-graph/` (`SQL`) for PostgreSQL 14–18. |
| `SLA violation: ...` (Exit code `1`) | Workload ran to completion, but a latency/TPS/recall/error SLA threshold failed | Inspect `$.sla_verifications` in `--json-report`. Do **not** treat as a binary crash. |
| Hang during `snb_bi.pgbench.json` | Ran `\phase_barrier` script with `-T <seconds>` | Replace `-T <seconds>` with bounded `-t <N>` (e.g., `-t 3`). |
