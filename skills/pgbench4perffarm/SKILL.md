---
name: pgbench4perffarm
description: >-
  Builds, initializes, executes, and validates postsql/pgbench4perffarm (standalone next-gen
  pgbench + contrib/pgbench C extension across PostgreSQL 14–20devel) and orchestrates
  continuous Perffarm animal setup, multi-commit sweeps, multi-duration benchmark sequences,
  and longitudinal SQL regression analysis. Use when building or testing pgbench4perffarm via
  PGXS, running TPC-inspired (pgbench-b, pgbench-c, pgbench-e, pgbench-h, pgbench-ds), HTAP
  (pgbench-htap), LDBC-inspired Graph (pgbench-graph, pgbench-graph-sql-ud, pgbench-graph-pgq),
  or VecBench-inspired Vector (pgbench-vec) workloads, enforcing --sla contracts, querying
  --json-report or --log-db telemetry, provisioning a Perffarm animal (perffarm/setup_machine.sh,
  provision_hosts.sh, setup_metrics_db.sh), running perffarm/run_perffarm.sh across single
  commits or --commits-file historical sweeps with --durations sequences, or querying
  perffarm_commit_summary_v, perffarm_regressions_v, and perffarm_compare_commits(). Don't use
  for non-PostgreSQL load generators or unrelated database benchmarks.
---

# PostgreSQL `pgbench4perffarm` & Perffarm Agentic Skill (`14`–`20devel`)

This skill equips AI coding and benchmarking agents to deterministically build, initialize, execute, and verify **`postsql/pgbench4perffarm`** across two core domains:
1. **Part A — Main `pgbench` & `contrib/pgbench` Usage**: Standalone `PGXS` build/install across PostgreSQL 14–20devel, single-command `--config=<file>.pgbench.json` workflows, 3+1 manual initialization stages, multi-group (`--group`), reservoir (`--reservoir`), phased barrier (`\phase_barrier`), causal watermark, result validation (`\validate_result`), and recall (`\record_recall`) execution, `--sla` exit-code triage, `--json-report` telemetry, `--log-db` SQL set-returning functions (SRFs), and `viewer/pgbench_viewer.py`.
2. **Part B — Perffarm Animal Setup & Continuous Runner (`perffarm/`)**: Automated host & disk discovery (`setup_machine.sh`), remote provisioning (`provision_hosts.sh`), isolated Metrics DB bootstrap (`setup_metrics_db.sh --ensure`), end-to-end execution (`run_perffarm.sh`), multi-duration sequences (`--durations=60,60,60,300`) on a single prepared database, multi-commit historical sweeps (`--commits-file`, `ccache` + `.perffarm_cfg_sig`), 5-point run verification (`verify_test_run.py`), cross-animal `remote_metrics_db` replication (`upload_results.sh`), and longitudinal SQL regression views (`perffarm_commit_summary_v`, `perffarm_regressions_v`, `perffarm_compare_commits()`).

---

## Part A: Main `pgbench` & `contrib/pgbench` Usage

### A1. Fast Build, Install & Verification (`PGXS`)

`pgbench4perffarm` builds out-of-tree against **any PostgreSQL 14–20devel** installation using `pg_config`:

```bash
# 1. Build both contrib/pgbench (server C extension) and src/bin/pgbench (client binary)
make -C /path/to/pgbench4perffarm -j"$(nproc)" PG_CONFIG=/path/to/pg_config

# 2. Install binary, C extension (.so + .sql + .control), workloads/, and viewer/ into PG installation
make -C /path/to/pgbench4perffarm install PG_CONFIG=/path/to/pg_config

# 3. Run contrib/pgbench SQL regression suite (requires running PostgreSQL instance)
make -C /path/to/pgbench4perffarm installcheck PG_CONFIG=/path/to/pg_config PGPORT=5432

# 4. Run src/bin/pgbench Perl TAP integration suite
make -C /path/to/pgbench4perffarm tapcheck PG_CONFIG=/path/to/pg_config
```

- **Installed artifacts**:
  - Binary: `$(pg_config --bindir)/pgbench`
  - Extension: `$(pg_config --pkglibdir)/pgbench.so` and `$(pg_config --sharedir)/extension/pgbench*` (`CREATE EXTENSION pgbench;` installs 51 C functions in `SCHEMA pgbench`)
  - Workload suites & `.pgbench.json` configs: `$(pg_config --sharedir)/pgbench/workloads/` (or in-tree at `src/bin/pgbench/workloads/`)
  - WebAssembly Telemetry Viewer: `$(pg_config --sharedir)/pgbench/viewer/pgbench_viewer.py` (or in-tree at `src/bin/pgbench/viewer/pgbench_viewer.py`).

### A2. Intent-to-Workload Decision Matrix

All relative `--config` paths below are under `src/bin/pgbench/workloads/` (or `$(pg_config --sharedir)/pgbench/workloads/`):

| Agent / User Goal | Suite Directory | Declarative `--config` Manifest(s) | Required Extensions | Perffarm `--tests` Name(s) |
| :--- | :--- | :--- | :--- | :--- |
| **Built-in microbenchmarks** | *(built-in)* | `-b tpcb-like`, `-b select-only`, `-b simple-update` | *(none)* | `tpcb-like`, `select-only`, `simple-update` |
| **TPC-B-inspired stored-proc banking OLTP** | `pgbench-b/` | `pgbench-b/pgbench_b.pgbench.json` | `pgbench` | `pgbench-b` |
| **TPC-C-inspired 9-table wholesale OLTP (`nurand`, `\try`/`\catch` 1% rollback)** | `pgbench-c/` | `pgbench-c/pgbench_c.pgbench.json` | `pgbench` | `pgbench-c` |
| **TPC-E-inspired 16-table brokerage OLTP** | `pgbench-e/` | `pgbench-e/pgbench_e.pgbench.json` | `pgbench` | `pgbench-e` |
| **TPC-H-inspired 8-table decision support OLAP + refresh stream** | `pgbench-h/` | `pgbench-h/pgbench_h.pgbench.json` | `pgbench` | `pgbench-h` |
| **TPC-DS-inspired 13-table star-schema OLAP + maintenance** | `pgbench-ds/` | `pgbench-ds/pgbench_ds.pgbench.json` | `pgbench` | `pgbench-ds` |
| **CH-benCHmark-inspired HTAP (isolated `oltp` + `olap` groups)** | `pgbench-htap/` | `pgbench-htap/pgbench_htap.pgbench.json` | `pgbench` | `pgbench-htap` |
| **LDBC-inspired Graph (`SQL` recursive CTEs)** | `pgbench-graph/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **LDBC-inspired Graph (`SQL_UD` `UNION DISTINCT ON`)** | `pgbench-graph-sql-ud/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-sql-ud-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **LDBC-inspired Graph (ISO `SQL/PGQ` `GRAPH_TABLE`, PG 19+)** | `pgbench-graph-pgq/` | `lsqb.pgbench.json`, `snb_interactive.pgbench.json`, `snb_bi.pgbench.json`, `finbench.pgbench.json`, `graphalytics.pgbench.json` | `pgbench` | `pgbench-graph-pgq-lsqb`, `-snb-interactive`, `-snb-bi`, `-finbench`, `-graphalytics` |
| **VecBench-inspired Vector ANN & Index Aging (`Recall@K`)** | `pgbench-vec/` | `pgbench-vec/vec_hybrid.pgbench.json`, `pgbench-vec/vec_aging.pgbench.json` | `pgbench` *(+ optional `vector`)* | `pgbench-vec-hybrid`, `pgbench-vec-aging` |

### A3. Initialization State Machine (`--config -i` vs. Manual `3+1` Stages)

#### Preferred: Single-Command `--config` Initialization & Execution
Every `.pgbench.json` file declares `init_steps[]`, `groups[]`, `reservoirs[]`, and `sla[]`, and resolves script paths relative to the JSON file's directory:
```bash
W="/path/to/pgbench4perffarm/src/bin/pgbench/workloads"

# 1. Initialize schema -> parallel in-database data generation -> constraints (and ground truth for vec):
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -i -s 4 "$PGDATABASE"

# 2. Run benchmark with --json-report and --log-db telemetry streaming:
pgbench --config="$W/pgbench-c/pgbench_c.pgbench.json" -s 4 -c 8 -j 4 -T 30 -P 1 \
  --json-report=/tmp/pgbench_c.json \
  --log-db="dbname=perffarm_metrics" --run-label="c-candidate" "$PGDATABASE"
```

#### Manual `3+1` Initialization Stages (When Invoking `*_schema.sql` / `*_init.sql` Directly)
Never execute workload scripts before completing all required stages in order using **`-n -t 1`**:
1. **Stage 1 (`*_schema.sql`)**: `pgbench -n -t 1 -f "$W/pgbench-c/pgbench_c_schema.sql" "$DB"`
2. **Stage 2 (`*_init.sql`)**: `pgbench -n -t 1 -c 4 -j 4 -D scale=4 -D num_workers=4 -f "$W/pgbench-c/pgbench_c_init.sql" "$DB"`
3. **Stage 3 (`*_constraints.sql`)**: `pgbench -n -t 1 -f "$W/pgbench-c/pgbench_c_constraints.sql" "$DB"`
4. **Stage 4 (`pgbench_vec_gt_gen.sql`, `pgbench-vec` only)**: `pgbench -n -t 1 -D scale=1 -f "$W/pgbench-vec/pgbench_vec_gt_gen.sql" "$DB"`

### A4. Non-Negotiable CLI Guardrails & Exit Code Semantics

1. **NEVER combine `-i` with `-f`**: `-i` is strictly for built-in TPC-B (`pgbench -i -s 10`) or `--config=<file>.pgbench.json -i` (combining `-i` with `-f` fails with `pgbench: error: some of the specified options cannot be used in initialization (-i) mode`). When running manual `*_schema.sql` or `*_init.sql` files, use `-f <script> -n -t 1` **without `-i`**.
2. **Understand `--config` vs. `--group` vs. `-c`/`-j`/`-f` precedence**: When `--config` defines `groups[]`, passing `-c N` or `-j M` on the CLI proportionally scales the group client/thread counts while preserving group ratios. Passing `-f`, `-b`, or `--group` on the CLI clears any scripts/groups loaded from `--config` (`clearConfigScriptsAndGroups()`), and when multiple `--group` flags are passed on the CLI, total clients/threads are computed as the sum of each group's `clients`/`threads`.
3. **NEVER run `\phase_barrier` scripts (`snb_bi.pgbench.json`) with `-T <seconds>`**: Barrier synchronization requires bounded transaction counts (`-t <N>`) so all clients enter the exact same number of phases.
4. **Always use `\reservoir_sample` (never `\reservoir_pop`)**: The backslash meta-command is `\reservoir_sample <reservoir_name> <var_name> [<default_expr>]` and requires `--reservoir=<name>:<capacity>`.
5. **Distinguish Exit Code `1` (SLA Violation vs. Fatal Error)**:
   - `0`: Run completed and all `--sla` checks passed (`$.sla_all_passed == true`).
   - `1`: **Either** an `--sla` threshold failed (where `--json-report` IS written with `$.sla_all_passed == false` and `$.sla_verifications[*]`) **OR** a fatal CLI/connection/SQL syntax error occurred (`pg_fatal`, no valid JSON report). **Always inspect `--json-report` first when exit code is `1`.**
   - `2`: Client aborted mid-run (e.g., unhandled SQL error outside `\try`/`\catch`, failed expression, or barrier abort).
6. **Legal & Trademark Guardrails**: Always describe suites as *"TPC-B-inspired"*, *"TPC-C-inspired"*, *"TPC-E-inspired"*, *"TPC-H-inspired"*, *"TPC-DS-inspired"*, *"LDBC-inspired"*, or *"VecBench-inspired"*. Never claim official TPC/LDBC/VecBench certification.

---

## Part B: Perffarm Animal Setup & Continuous Runner (`perffarm/`)

### B1. Perffarm Architecture & Topologies

`perffarm/` provides a zero-external-dependency Bash + Python 3 orchestration harness supporting 4 deployment topologies configured in `perffarm/conf/animal.conf`:
- `TOPOLOGY="local"` (`SERVER_SSH_TARGET=""`, `CLIENT_SSH_TARGET=""`): Single host runs controller, PostgreSQL server, and `pgbench` client.
- `TOPOLOGY="remote-server"` (`SERVER_SSH_TARGET="user@db-host"`): Controller + `pgbench` client run locally; PostgreSQL server runs on `SERVER_SSH_TARGET`.
- `TOPOLOGY="remote-client"` (`CLIENT_SSH_TARGET="user@load-host"`): Controller + PostgreSQL server run locally; `pgbench` runs on `CLIENT_SSH_TARGET`.
- `TOPOLOGY="both-remote"` (`SERVER_SSH_TARGET="user@db-host"`, `CLIENT_SSH_TARGET="user@load-host"`): Controller orchestrates remote server and client VMs over SSH.

### B2. Step-by-Step Setup & Provisioning Workflow

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
# Recipe 1: Fast smoke verification (~30-60s) using an existing PostgreSQL installation
./perffarm/run_perffarm.sh \
  --config=perffarm/conf/animal.conf \
  --prebuilt-pg-config=/path/to/pg_config \
  --profile=smoke \
  --tests=tpcb-like,pgbench-b,pgbench-c \
  --durations=2,2 \
  --fast-check \
  --force

# Recipe 2: Multi-duration sequence on a SINGLE prepared database (3x60s warmup/variance + 1x300s steady-state)
./perffarm/run_perffarm.sh \
  --config=perffarm/conf/animal.conf \
  --profile=standard \
  --tests=tpcb-like,pgbench-b,pgbench-c,pgbench-htap \
  --durations=60,60,60,300 \
  --force

# Recipe 3: Multi-commit historical sweep (1 commit/24h over last 31 or 90 days, green GitHub CI only)
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

- **Key Multi-Commit & Multi-Duration Mechanics**:
  - `--durations=60,60,60,300` (or `60x3,300` / `--repeats=3`) initializes the database **once** per test suite, then runs each timed step sequentially against that same prepared database without re-initializing, recording `sequence_run_index` (`1..N`), `sequence_total_runs`, `target_duration_sec`, and `prepared_db_reused` (`false` on step 1, `true` on steps `2..N`) in `perffarm_test_runs.verification_details`.
  - When `--commits-file` or `--commits` is passed without `--force`, `run_perffarm.sh` checks `perffarm_batches` in `METRICS_DB_CONNINFO` **before** building PostgreSQL and skips commits already marked `completed` for `(animal_name, pg_branch, test_profile)`.
  - `USE_CCACHE="auto"` and `.perffarm_cfg_sig` reuse the configured build tree across commits so each incremental commit build takes ~15–25s instead of ~2 minutes.

### B4. The 5-Point Verification Contract (`perffarm/verify_test_run.py`)

Every test run in every batch is validated against 5 invariants and recorded in `perffarm_test_runs`:
1. **`1_exit_code_zero`**: `pgbench` `exit_code == 0` and JSON report is present.
2. **`2_transactions_healthy`**: `transactions_processed > 0` and unexpected `transactions_failed == 0`.
3. **`3_result_validation_100pct`**: 100% `\validate_result` pass rate (`validation_failed == 0`).
4. **`4_sla_compliance`**: All `--sla` checks passed (`sla_all_passed == true`).
5. **`5_server_log_clean`**: `postgres.log` contains zero `PANIC:`, `TRAP:`, `FailedAssertion`, `Segmentation fault`, unexpected `FATAL:`, `cache reference leak`, `buffer refcount leak`, `TupleDesc reference leak`, `Snapshot reference leak`, or `relcache reference leak`.

### B5. Longitudinal SQL Regression Views & Cross-Commit Comparison (`perffarm_metrics`)

Query `METRICS_DB_CONNINFO` directly with `psql` to inspect runs and detect regressions across commits:

```sql
-- 1. Single-run scorecard or 2-run side-by-side comparison (built into metrics_db_schema.sql)
SELECT * FROM pgbench_show_run();
SELECT * FROM pgbench_compare_runs(101, 105);

-- 2. Longitudinal commit summary with CPU/RAM/disk inventory, SELECT 1 & 1MB calibration, and sequence step metadata
SELECT batch_id, animal_name, commit_short, git_commit_ts::date AS commit_date,
       test_name, run_seq_idx, target_duration_sec, prepared_db_reused,
       verification_status, tps, latency_p95_ms, latency_p99_ms,
       select1_avg_ms, repeat1mb_mb_per_sec, buffer_hit_ratio_pct
FROM perffarm_commit_summary_v
WHERE test_name = 'pgbench-c'
ORDER BY batch_id DESC, run_seq_idx ASC;

-- 3. Commit-over-commit regression view (partitioned by animal_name, pg_branch, test_profile, test_name, run_seq_idx, target_duration_sec)
SELECT animal_name, test_name, run_seq_idx, target_duration_sec,
       prev_commit_short, commit_short, prev_tps, tps, tps_pct_change,
       prev_latency_p95_ms, latency_p95_ms, latency_p95_pct_change, verdict
FROM perffarm_regressions_v
WHERE verdict IN ('REGRESSED', 'FAILED_VERIFICATION', 'IMPROVED');

-- 4. Compare any two git commit SHAs (prefix match supported) across all common tests & sequence steps
SELECT * FROM perffarm_compare_commits('2c10c2ce4d', 'dfd7ede529');
-- Optional filters: perffarm_compare_commits(sha_a, sha_b, p_animal := 'lynx-x86-nvme', p_suite_filter := 'pgbench-c')
```

---

## Progressive Disclosure References

Load only the specific reference file needed for your active sub-task:

1. **[Main `pgbench` Usage, Fast Test Plan & Execution Recipes](references/pgbench_main_usage_and_recipes.md)**
   - Load when initializing or running any of the 8 workloads (`pgbench-b/c/e/h/ds/htap/graph/vec`), cloning template databases, running the fast verification script, or triaging `pgbench` exit codes and `--json-report` assertions.
2. **[Perffarm Animal Setup, Runner Flags & SQL Regression Cookbook](references/perffarm_setup_and_runner.md)**
   - Load when configuring `animal.conf`, running `setup_machine.sh` / `provision_hosts.sh` / `setup_metrics_db.sh` / `run_perffarm.sh`, executing multi-commit `--commits-file` sweeps, configuring cross-animal `REMOTE_METRICS_DB_CONNINFO` replication, or debugging `verify_test_run.py` failures.
3. **[Complete CLI Flags, Backslash Commands, Client & `contrib/pgbench` SQL Functions Reference](references/cli_and_functions_reference.md)**
   - Load when authoring custom `.sql` workload scripts, `.pgbench.json` configurations, `--sla` expressions, client expression functions (61 functions), backslash meta-commands (39 commands), or `contrib/pgbench` server C functions (51 SQL functions in `SCHEMA pgbench` + `--log-db` SRFs).
