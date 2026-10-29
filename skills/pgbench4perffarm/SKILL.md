---
# INSPIRATION & TRADEMARK DISCLAIMER:
# The benchmark workloads in this repository are clean-room, non-comparable engineering workloads
# historically inspired by industry benchmarks (such as TPC-B, TPC-C, TPC-E, TPC-H, TPC-DS,
# CH-benCHmark, LDBC, and VecBench). Those names are mentioned ONLY in this opening header comment
# to acknowledge architectural inspiration. Throughout all file names, titles, documentation,
# configurations, and scripts, always use the clean-room `pgbench-*` workload names.
name: pgbench4perffarm
description: >-
  Builds, initializes, executes, and validates pgbench4perffarm (in Google3 at
  //third_party/pgext/pgbench4perffarm via blaze-for-agents, or standalone postsql/pgbench4perffarm
  via PGXS across PostgreSQL 14–20devel) and orchestrates continuous Perffarm animal setup,
  multi-commit sweeps, multi-duration benchmark sequences, and longitudinal SQL regression
  analysis. Use when building or testing pgbench4perffarm via Blaze or PGXS, running
  relational OLTP/OLAP (pgbench-banking / pgbench-b, pgbench-order / pgbench-c, pgbench-brokerage / pgbench-e,
  pgbench-analytics / pgbench-h, pgbench-retail / pgbench-ds), HTAP (pgbench-htap),
  Graph (pgbench-graph, pgbench-graph-sql-ud, pgbench-graph-pgq, pgbench-graph-plpgq), or
  Vector (pgbench-vec) workloads, enforcing --sla contracts, querying
  --json-report or --log-db telemetry, provisioning a Perffarm animal (perffarm/setup_machine.sh,
  provision_hosts.sh, setup_metrics_db.sh), running perffarm/run_perffarm.sh across single
  commits or --commits-file historical sweeps with --durations sequences, or querying
  perffarm_commit_summary_v, perffarm_regressions_v, and perffarm_compare_commits(). Don't use
  for non-PostgreSQL load generators or unrelated database benchmarks.
---

# PostgreSQL `pgbench4perffarm` & Perffarm Agentic Skill (Google3 & `PGXS` `14`–`20devel`)

This skill equips **Jetski**, Google3 subagents (`DeepCoder`, `invoke_subagent`, `swarm`), and external AI coding/benchmarking agents to deterministically build, initialize, execute, and verify **`pgbench4perffarm`** across two core domains:
1. **Part A — Main `pgbench` & `contrib/pgbench` Usage**: Google3 Blaze builds/tests (`//third_party/pgext/pgbench4perffarm` via `blaze-for-agents`) and standalone `PGXS` build/install across PostgreSQL 14–20devel, single-command `--config=<file>.pgbench.json` workflows, 3+1 manual initialization stages, multi-group (`--group`), reservoir (`--reservoir`), phased barrier (`\phase_barrier`), causal watermark, result validation (`\validate_result`), and recall (`\record_recall`) execution, `--sla` exit-code triage, `--json-report` telemetry, `--log-db` SQL set-returning functions (SRFs), and `viewer/pgbench_viewer.py`.
2. **Part B — Perffarm Animal Setup & Continuous Runner (`perffarm/`)**: Automated host & disk discovery (`setup_machine.sh`), remote provisioning (`provision_hosts.sh`), isolated Metrics DB bootstrap (`setup_metrics_db.sh --ensure`), end-to-end execution (`run_perffarm.sh`), multi-duration sequences (`--durations=60,60,60,300`) on a single prepared database, multi-commit historical sweeps (`--commits-file`, `ccache` + `.perffarm_cfg_sig`), 5-point run verification (`verify_test_run.py`), cross-animal `remote_metrics_db` replication (`upload_results.sh`), and longitudinal SQL regression views (`perffarm_commit_summary_v`, `perffarm_regressions_v`, `perffarm_compare_commits()`).

---

## Part A: Main `pgbench` & `contrib/pgbench` Usage

### A1. Fast Build, Install & Verification (Google3 Blaze & Standalone `PGXS`)

#### 1. Google3 Build & Test via `blaze-for-agents` (`//third_party/pgext/pgbench4perffarm`)
In a Google3 CitC workspace, always use `/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents` (never bare `blaze` or `bazel`), and never run `find`/`grep`/`rg` in `google3/` (use `code_search`):

```bash
# Test unified package (pgbench.so SQL regression + pgbench client/viewer/perffarm test)
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents test \
  //third_party/pgext/pgbench4perffarm:postgres18gce/regress \
  //third_party/pgext/pgbench4perffarm:postgres18gce/pgbench4perffarm_test

# Build across all supported Cloud SQL (postgres12gce..postgresmastergce) & AlloyDB versions
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents build \
  //third_party/pgext/pgbench4perffarm:all
```

#### 2. Standalone Out-of-Tree Build & Test via `PGXS` (PostgreSQL 14–20devel)
On Cloudtop in the standalone `PGXS` repository (e.g., `/usr/local/google/home/hannuk/pgbench4perffarm` with `PG_CONFIG=/usr/local/google/home/hannuk/pg20_install/bin/pg_config`; note that `//third_party/pgext/pgbench4perffarm` in Google3 builds exclusively via Blaze `BUILD` and never contains `Makefile` or `meson.build` files):

```bash
PG_CONFIG="${PG_CONFIG:-/usr/local/google/home/hannuk/pg20_install/bin/pg_config}"
P4P_DIR="${P4P_DIR:-/usr/local/google/home/hannuk/pgbench4perffarm}"

# 1. Build both contrib/pgbench (server C extension) and src/bin/pgbench (client binary)
make -C "$P4P_DIR" -j"$(nproc)" PG_CONFIG="$PG_CONFIG"

# 2. Install binary, C extension (.so + .sql + .control), workloads/, and viewer/ into PG installation
make -C "$P4P_DIR" install PG_CONFIG="$PG_CONFIG"

# 3. Run contrib/pgbench SQL regression suite (requires running PostgreSQL instance)
make -C "$P4P_DIR" installcheck PG_CONFIG="$PG_CONFIG" PGPORT=5432

# 4. Run src/bin/pgbench Perl TAP integration suite
make -C "$P4P_DIR" tapcheck PG_CONFIG="$PG_CONFIG"
```

- **Installed artifacts**:
  - Binary: `$(pg_config --bindir)/pgbench` (or `blaze-bin/third_party/pgext/pgbench4perffarm/postgres18gce/pgbench`)
  - Extension: `$(pg_config --pkglibdir)/pgbench.so` and `$(pg_config --sharedir)/extension/pgbench*` (`CREATE EXTENSION pgbench;` installs 51 C functions in `SCHEMA pgbench`)
  - Workload suites & `.pgbench.json` configs: `$(pg_config --sharedir)/pgbench/workloads/` (or in-tree at `src/bin/pgbench/workloads/`)
  - WebAssembly Telemetry Viewer: `$(pg_config --sharedir)/pgbench/viewer/pgbench_viewer.py` (or in-tree at `src/bin/pgbench/viewer/pgbench_viewer.py`).

### A2. Intent-to-Workload Decision Matrix

All relative `--config` paths below are under `src/bin/pgbench/workloads/` (or `$(pg_config --sharedir)/pgbench/workloads/`):

| Agent / User Goal | Suite Directory | Declarative `--config` Manifest(s) | Required Extensions | Perffarm `--tests` Name(s) |
| :--- | :--- | :--- | :--- | :--- |
| **Built-in microbenchmarks** | *(built-in)* | `-b tpcb-like`, `-b select-only`, `-b simple-update` | *(none)* | `tpcb-like`, `select-only`, `simple-update` |
| **Stored-procedure banking OLTP (`pgbench-banking` / `pgbench-b`)** | `pgbench-banking/` (`pgbench-b/`) | `pgbench-banking/pgbench_banking.pgbench.json` (`pgbench-b/pgbench_b.pgbench.json`) | `pgbench` | `pgbench-b` |
| **9-table wholesale order-entry OLTP (`pgbench-order` / `pgbench-c`, `nurand`, `\try`/`\catch` 1% rollback)** | `pgbench-order/` (`pgbench-c/`) | `pgbench-order/pgbench_order.pgbench.json` (`pgbench-c/pgbench_c.pgbench.json`) | `pgbench` | `pgbench-c` |
| **16-table financial brokerage OLTP (`pgbench-brokerage` / `pgbench-e`)** | `pgbench-brokerage/` (`pgbench-e/`) | `pgbench-brokerage/pgbench_brokerage.pgbench.json` (`pgbench-e/pgbench_e.pgbench.json`) | `pgbench` | `pgbench-e` |
| **8-table decision support OLAP + refresh stream (`pgbench-analytics` / `pgbench-h`)** | `pgbench-analytics/` (`pgbench-h/`) | `pgbench-analytics/pgbench_analytics.pgbench.json` (`pgbench-h/pgbench_h.pgbench.json`) | `pgbench` | `pgbench-h` |
| **13-table star-schema retail OLAP + maintenance (`pgbench-retail` / `pgbench-ds`)** | `pgbench-retail/` (`pgbench-ds/`) | `pgbench-retail/pgbench_retail.pgbench.json` (`pgbench-ds/pgbench_ds.pgbench.json`) | `pgbench` | `pgbench-ds` |
| **Hybrid OLTP + OLAP with isolated `oltp` + `olap` groups (`pgbench-htap`)** | `pgbench-htap/` | `pgbench-htap/pgbench_htap.pgbench.json` | `pgbench` | `pgbench-htap` |
| **Property Graph (`SQL` recursive CTEs)** | `pgbench-graph/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **Property Graph (`SQL_UD` `UNION DISTINCT ON`)** | `pgbench-graph-sql-ud/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-sql-ud-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **Property Graph (ISO `SQL/PGQ` `GRAPH_TABLE`, PG 19+)** | `pgbench-graph-pgq/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-pgq-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **Property Graph (`plPGQ` via `plv8` / `pljs`)** | `pgbench-graph-plpgq/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench`, `plv8` or `pljs` | *(standalone `--config`)* |
| **Vector ANN & Index Aging (`Recall@K`)** | `pgbench-vec/` | `pgbench-vec/vec_hybrid.pgbench.json`, `pgbench-vec/vec_aging.pgbench.json` | `pgbench` *(+ optional `vector`)* | `pgbench-vec-hybrid`, `pgbench-vec-aging` |

### A3. Initialization State Machine (`--config -i` vs. Manual `3+1` Stages)

#### Preferred: Single-Command `--config` Initialization & Execution
Every `.pgbench.json` file declares `init_steps[]`, `groups[]`, `reservoirs[]`, and `sla[]`, and resolves script paths relative to the JSON file's directory:
```bash
W="src/bin/pgbench/workloads"

# 1. Initialize schema -> parallel in-database data generation -> constraints (and ground truth for vec):
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -i -s 4 "$PGDATABASE"

# 2. Run benchmark with --json-report and --log-db telemetry streaming:
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -s 4 -c 8 -j 4 -T 30 -P 1 \
  --json-report=/tmp/pgbench_c.json \
  --log-db="dbname=perffarm_metrics" --run-label="c-candidate" "$PGDATABASE"
```

#### Manual `3+1` Initialization Stages (When Invoking `*_schema.sql` / `*_init.sql` Directly)
Never execute workload scripts before completing all required stages in order using **`-n -t 1`**:
1. **Stage 1 (`*_schema.sql`)**: `pgbench -n -t 1 -f "$W/pgbench-order/pgbench_order_schema.sql" "$DB"`
2. **Stage 2 (`*_init.sql`)**: `pgbench -n -t 1 -c 4 -j 4 -D scale=4 -D num_workers=4 -f "$W/pgbench-order/pgbench_order_init.sql" "$DB"`
3. **Stage 3 (`*_constraints.sql`)**: `pgbench -n -t 1 -f "$W/pgbench-order/pgbench_order_constraints.sql" "$DB"`
4. **Stage 4 (`pgbench_vec_gt_gen.sql`, `pgbench-vec` only)**: `pgbench -n -t 1 -D scale=1 -f "$W/pgbench-vec/pgbench_vec_gt_gen.sql" "$DB"`

### A4. Non-Negotiable CLI Guardrails & Exit Code Semantics

1. **NEVER combine `-i` with `-f`**: `-i` is strictly for built-in banking (`pgbench -i -s 10`) or `--config=<file>.pgbench.json -i` (combining `-i` with `-f` fails with `pgbench: error: some of the specified options cannot be used in initialization (-i) mode`). When running manual `*_schema.sql` or `*_init.sql` files, use `-f <script> -n -t 1` **without `-i`**.
2. **Understand `--config` vs. `--group` vs. `-c`/`-j`/`-f` precedence**: When `--config` defines `groups[]`, passing `-c N` or `-j M` on the CLI proportionally scales the group client/thread counts while preserving group ratios. Passing `-f`, `-b`, or `--group` on the CLI clears any scripts/groups loaded from `--config` (`clearConfigScriptsAndGroups()`), and when multiple `--group` flags are passed on the CLI, total clients/threads are computed as the sum of each group's `clients`/`threads`.
3. **NEVER run `\phase_barrier` scripts (`snb_bi.pgbench.json`) with `-T <seconds>`**: Barrier synchronization requires bounded transaction counts (`-t <N>`) so all clients enter the exact same number of phases.
4. **Always use `\reservoir_sample` (never `\reservoir_pop`)**: The backslash meta-command is `\reservoir_sample <reservoir_name> <var_name> [<default_expr>]` and requires `--reservoir=<name>:<capacity>`.
5. **Distinguish Exit Code `1` (SLA Violation vs. Fatal Error)**:
   - `0`: Run completed and all `--sla` checks passed (`$.sla_all_passed == true`).
   - `1`: **Either** an `--sla` threshold failed (where `--json-report` IS written with `$.sla_all_passed == false` and `$.sla_verifications[*]`) **OR** a fatal CLI/connection/SQL syntax error occurred (`pg_fatal`, no valid JSON report). **Always inspect `--json-report` first when exit code is `1`.**
   - `2`: Client aborted mid-run (e.g., unhandled SQL error outside `\try`/`\catch`, failed expression, or barrier abort).
6. **CitC Hygiene, `//third_party/pgext/` Packaging & Clean-Room Naming Guardrails**:
   - Never run `find`/`grep` in `google3/` (use `code_search`), and never write Perffarm `$WORKDIR` or `--json-report` files into CitC (`/google/src/cloud/...`).
   - Never upload or commit `Makefile` or `meson.build` files into `//third_party/pgext/` in Google3 (keep them only in standalone `PGXS` repositories and exclude them in `copy.bara.sky`), and always include a top-level `LICENSE` file containing the full PostgreSQL License text (referenced by `default_applicable_licenses = [":license"]` in `BUILD`).
   - Always use our preferred clean-room benchmark names in filenames, titles, headings, prose, and script names (`pgbench-banking` / `pgbench-b`, `pgbench-order` / `pgbench-c`, `pgbench-brokerage` / `pgbench-e`, `pgbench-analytics` / `pgbench-h`, `pgbench-retail` / `pgbench-ds`, `pgbench-htap`, `pgbench-graph`, `pgbench-vec`). Mention historical industry benchmark inspirations only in the opening file-header comment (`src/bin/pgbench/workloads/README` and `src/bin/pgbench/workloads/BENCHMARK_INSPIRATION.md`), and never claim official benchmark certification.

---

## Part B: Perffarm Animal Setup & Continuous Runner (`perffarm/`)

### B1. Perffarm Architecture & Topologies

`perffarm/` provides a zero-external-dependency Bash + Python 3 orchestration harness supporting 4 deployment topologies configured in `perffarm/conf/animal.conf`:
- `TOPOLOGY="local"` (`SERVER_SSH_TARGET=""`, `CLIENT_SSH_TARGET=""`): Single host runs controller, PostgreSQL server, and `pgbench` client.
- `TOPOLOGY="remote-server"` (`SERVER_SSH_TARGET="user@db-host"`): Controller + `pgbench` client run locally; PostgreSQL server runs on `SERVER_SSH_TARGET`.
- `TOPOLOGY="remote-client"` (`CLIENT_SSH_TARGET="user@load-host"`): Controller + PostgreSQL server run locally; `pgbench` runs on `CLIENT_SSH_TARGET`.
- `TOPOLOGY="both-remote"` (`SERVER_SSH_TARGET="user@db-host"`, `CLIENT_SSH_TARGET="user@load-host"`): Controller orchestrates remote server and client VMs over SSH.

### B2. Step-by-Step Setup & Provisioning Workflow

> **Important for CitC Workspaces**: When running `perffarm/` from a CitC workspace, always pass `--config=/var/tmp/perffarm/animal.conf` and `WORKDIR=/var/tmp/perffarm/work` (or run from `/usr/local/google/home/hannuk/pgbench4perffarm`) so runtime state and database files are stored on local disk outside CitC.

```bash
# Step 1: Initialize animal.conf (if missing), probe CPU/RAM & separate system/data/WAL disks, and generate postgresql.animal.conf
./perffarm/setup_machine.sh --config=perffarm/conf/animal.conf --init-config

# (Optional) Step 1b: Verify / provision external or Cloud VM server/client hosts over SSH (subcommands: verify-ssh | provision | sync-repo | collect-info | teardown | status)
./perffarm/provision_hosts.sh verify-ssh --config=perffarm/conf/animal.conf

# Step 2: Bootstrap persistent Metrics DB (maintains isolated $WORKDIR/metrics_pg_install so SUT rebuilds never clobber it)
./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure
./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --status
```

### B3. Running `perffarm/run_perffarm.sh` (Single Run, Multi-Duration Sequences & Historical Sweeps)

`perffarm/run_perffarm.sh` executes the complete Perffarm pipeline (`setup_machine.sh` + `setup_metrics_db.sh --ensure` -> Git checkout + CI check + commit deduplication -> PostgreSQL + `pgbench4perffarm` build + `check-world` -> `calibrate_baseline.py` (`SELECT 1` + `1MB` `repeat('0123456789', 100000)`) -> per-test initialization & timed execution -> 5-point `verify_test_run.py` -> `upload_results.sh`):

```bash
# Recipe 1: Fast smoke verification (~30-60s) using an existing PostgreSQL installation (e.g. Cloudtop pg20_install)
./perffarm/run_perffarm.sh \
  --config=perffarm/conf/animal.conf \
  --prebuilt-pg-config=/usr/local/google/home/hannuk/pg20_install/bin/pg_config \
  --profile=smoke \
  --tests=tpcb-like,pgbench-b,pgbench-c \
  --durations=2,2 \
  --fast-check \
  --force

# Recipe 2: Multi-duration sequence on a single prepared DB (3x60s warmup + 1x300s steady-state without re-initializing!)
./perffarm/run_perffarm.sh \
  --config=perffarm/conf/animal.conf \
  --branch=master \
  --profile=standard \
  --tests=pgbench-c \
  --durations=60,60,60,300 \
  --fast-check \
  --force

# Recipe 3: Historical multi-commit sweep (1 commit per 24h over last month or 90 days) with ccache + automatic deduplication
./perffarm/run_perffarm.sh \
  --config=perffarm/conf/animal.conf \
  --branch=master \
  --commits-file=perffarm/conf/commits_master_24h_last_month.txt \
  --ci-status=github_success \
  --profile=standard \
  --tests=pgbench-c \
  --durations=60,60,60,300 \
  --fast-check
```

### B4. 5-Point Run Verification & Longitudinal SQL Regression Analysis

Every test run is checked by `perffarm/verify_test_run.py` across 5 gates (`1_exit_code_zero`, `2_transactions_healthy`, `3_result_validation_100pct`, `4_sla_compliance`, `5_server_log_clean`) and stored in `perffarm_metrics` (default `host=127.0.0.1 port=55433 dbname=perffarm_metrics`):

```sql
-- 1. Inspect the most recent pgbench run scorecard (or pass a specific run_id / run_label)
SELECT * FROM pgbench_show_run();

-- 2. Longitudinal commit summary (partitioned by run_seq_idx & target_duration_sec)
SELECT commit_short, git_commit_ts::date AS dt, test_name,
       run_seq_idx, target_duration_sec, prepared_db_reused,
       tps, latency_p50_ms, latency_p95_ms, latency_p99_ms, verification_status
FROM perffarm_commit_summary_v
WHERE test_name = 'pgbench-c' AND target_duration_sec = 300
ORDER BY git_commit_ts DESC;

-- 3. Automatic commit-over-commit regression detection (compares matching run_seq_idx & target_duration_sec)
SELECT animal_name, test_name, run_seq_idx, target_duration_sec,
       prev_commit_short, commit_short, prev_tps, tps, tps_pct_change,
       prev_latency_p95_ms, latency_p95_ms, latency_p95_pct_change, verdict
FROM perffarm_regressions_v
WHERE verdict IN ('REGRESSED', 'FAILED_VERIFICATION', 'IMPROVED');

-- 4. Compare any two git commit SHAs (prefix match supported) across all common tests & sequence steps
SELECT * FROM perffarm_compare_commits('f197fdee82', '3c5d9d914f');
-- Optional filters: perffarm_compare_commits(sha_a, sha_b, p_animal := 'dev02_dev_e4a', p_suite_filter := 'pgbench-c')
```

---

## Progressive Disclosure References

Load only the specific reference file needed for your active sub-task:

1. **[Main `pgbench` Usage, Fast Test Plan & Execution Recipes](references/pgbench_main_usage_and_recipes.md)**
   - Load when building/testing via Google3 `blaze-for-agents` or `PGXS`, initializing or running any of the 8 workloads (`pgbench-b/c/e/h/ds/htap/graph/vec`), cloning template databases, running the fast verification script, or triaging `pgbench` exit codes and `--json-report` assertions.
2. **[Perffarm Animal Setup, Runner Flags & SQL Regression Cookbook](references/perffarm_setup_and_runner.md)**
   - Load when configuring `animal.conf`, running `setup_machine.sh` / `provision_hosts.sh` / `setup_metrics_db.sh` / `run_perffarm.sh`, executing multi-commit `--commits-file` sweeps, configuring cross-animal `REMOTE_METRICS_DB_CONNINFO` replication, or debugging `verify_test_run.py` failures.
3. **[Wholesale Order-Entry (`pgbench-order` / `pgbench-c`) Specification & Multi-Dialect / Non-SQL Porting Reference](references/pgbench_order_workflow_and_porting_spec.md)** (companion repo guide: `doc/agent-guide-pgbench-order.md`)
   - Load when re-creating, porting, or adapting the 9-table wholesale order-entry workload (`New-Order`, `Payment`, `Order-Status`, `Delivery`, `Stock-Level`, `nurand`/`c_last`/`permute` distributions, and 8 post-run consistency invariants) using either `Model A` (server-side `PL/pgSQL` stored procedures + `contrib/pgbench` C functions, with full `Model A` vs. `Model B` performance analysis), `Model B` (pure client-driven SQL DML for Cloud Spanner GoogleSQL, MySQL, DuckDB, SQLite, T-SQL, PL/SQL), or `Model C` (Neo4j Cypher, MongoDB MQL, Key-Value stores).
4. **[Complete CLI Flags, Backslash Commands, Client & `contrib/pgbench` SQL Functions Reference](references/cli_and_functions_reference.md)**
   - Load when authoring custom `.sql` workload scripts, `.pgbench.json` configurations, `--sla` expressions, client expression functions (61 functions), backslash meta-commands (39 commands), or `contrib/pgbench` server C functions (51 SQL functions in `SCHEMA pgbench` + `--log-db` SRFs).
