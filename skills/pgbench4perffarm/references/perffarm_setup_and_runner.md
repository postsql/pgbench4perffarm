# Perffarm Animal Setup, Runner Reference & Longitudinal SQL Cookbook

This reference covers the complete operational lifecycle of the **`perffarm/`** continuous performance benchmarking and historical commit-sweep harness in `postsql/pgbench4perffarm`.

---

## 1. Perffarm Agent State Machine

Agents operating `perffarm/` transition through these deterministic states in order:

```
[1. UNCONFIGURED]
   │  ./perffarm/setup_machine.sh --config=perffarm/conf/animal.conf --init-config
   ▼
[2. HOST_DISCOVERED]  (animal.conf + postgresql.animal.conf + server/client_instance_info.json + hardware_profile.json)
   │  ./perffarm/provision_hosts.sh verify-ssh --config=perffarm/conf/animal.conf  (if remote SSH/Cloud VM)
   │  ./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure
   ▼
[3. METRICS_DB_READY] (Persistent perffarm_metrics DB running on isolated $WORKDIR/metrics_pg_install + schema installed)
   │  ./perffarm/run_perffarm.sh --config=perffarm/conf/animal.conf [options]
   ▼
[4. COMMIT_LOOP & DEDUP] (Checks perffarm_batches; skips already-completed commits unless --force)
   │  ccache + .perffarm_cfg_sig incremental PG build -> PGXS install of pgbench4perffarm -> check-world
   ▼
[5. CALIBRATED] (calibrate_baseline.py measures SELECT 1 & 1MB repeat('0123456789', 100000) round-trip/throughput)
   │  For each test in test_matrix.json: 1x Init (--config -i) -> N sequential timed runs (--durations=d1,d2,...,dN)
   ▼
[6. FIVE_POINT_VERIFIED] (verify_test_run.py records perffarm_test_runs & checks exit_code, tx_healthy, validation, SLA, log_clean)
   │  upload_results.sh packages batch archive + replicates to optional REMOTE_METRICS_DB_CONNINFO
   ▼
[7. LONGITUDINAL_ANALYSIS] (Query perffarm_commit_summary_v, perffarm_regressions_v, perffarm_compare_commits())
```

---

## 2. Complete `perffarm/` Script CLI Options

### 2.1 `perffarm/setup_machine.sh`
| Flag | Description |
| :--- | :--- |
| `--config=PATH` | Path to `animal.conf` (default: `perffarm/conf/animal.conf`). |
| `--init-config` | Copy `perffarm/conf/animal.conf.example` to `--config` if missing. |
| `--install-deps` | Install OS build & TAP test dependencies via `sudo apt-get` or `dnf`. |
| `--tune-os` | Apply Linux `sysctl`, CPU governor (`performance`), and THP tuning via `sudo`. |
| `--no-io-probe` | Skip the 16 MB sequential write + `fsync` disk micro-probe. |
| `--preset=TIER` | Override `PG_SIZING_PRESET` (`auto` \| `small-dev` \| `medium-vm` \| `large-baremetal`). |
| `--pg-version=MAJOR` | Target PostgreSQL major version for GUC filtering (default: `20`). |

### 2.2 `perffarm/setup_metrics_db.sh`
| Flag | Description |
| :--- | :--- |
| `--config=PATH` | Path to `animal.conf` (default: `perffarm/conf/animal.conf`). |
| `--pg-config=PATH` | Explicit `pg_config` binary to use for the local Metrics DB cluster. |
| `--ensure`, `--start` | Initialize (if needed), start local Metrics DB, apply `sql/metrics_db_schema.sql`, and upsert `perffarm_animals` (default). |
| `--status` | Show `pg_isready` status and row counts for `perffarm_animals`, `perffarm_batches`, `perffarm_test_runs`, and `pgbench_runs`. |
| `--stop` | Stop the local Metrics DB cluster. |
| `--reset` | Stop, wipe, and re-initialize the local Metrics DB cluster from scratch. |

### 2.3 `perffarm/run_perffarm.sh`
| Flag | Description |
| :--- | :--- |
| `--config=PATH` | Path to `animal.conf` (default: `perffarm/conf/animal.conf`). |
| `--branch=NAME` | Override `PG_BRANCHES` (e.g., `master`, `REL_17_STABLE`). |
| `--commit=SHA` | Check out and benchmark a single specific git commit SHA/ref. |
| `--commits=S1,S2,...` | Comma- or space-separated list of commits to benchmark sequentially. |
| `--commits-file=PATH` | File with one commit SHA per line (`#` comments and trailing tokens ignored). Pre-built lists: `perffarm/conf/commits_master_24h_last_month.txt` (31 commits) and `perffarm/conf/commits_master_24h_last_90d.txt` (90 commits). |
| `--ci-status=STATUS` | Explicit CI status (e.g., `github_success`) when pre-verified. |
| `--verify-ci=MODE` | `auto` \| `strict` \| `off` (alias `--skip-ci-check`). |
| `--profile=TIER` | Sizing profile from `conf/test_matrix.json`: `smoke` (`5-6s`), `standard` (`60s`), `nightly` (`180-300s`). |
| `--tests=LIST` | Comma-separated test or suite names from `conf/test_matrix.json`, or `all`. |
| `--durations=LIST` | Multi-duration sequence (seconds) run sequentially against each **single prepared DB** without re-initializing (e.g., `60,60,60,300` or `60x3,300`). Alias: `--duration-sequence=LIST`. |
| `--repeats=N` | Number of consecutive runs per prepared DB when `--durations` is not set. |
| `--scale=N` | Override benchmark scale factor. |
| `--clients=N` | Override benchmark client count. |
| `--threads=N` | Override benchmark worker thread count. |
| `--duration=SEC` | Override single-run benchmark duration in seconds. |
| `--metrics-level=LVL` | Telemetry granularity: `summary` \| `timeseries` \| `full-trace`. |
| `--upload-level=LVL` | Upload granularity: `summary` \| `timeseries` \| `full-trace`. |
| `--topology=MODE` | `local` \| `remote-server` \| `remote-client` \| `both-remote`. |
| `--server-ssh=U@H` | Remote PostgreSQL server SSH target. |
| `--client-ssh=U@H` | Remote `pgbench` client SSH target. |
| `--prebuilt-pg-config=P`| Skip cloning/compiling PostgreSQL; use an existing `pg_config` binary and install `pgbench4perffarm` into it. |
| `--local-pg-src=DIR` | Use an existing local PostgreSQL git checkout (or reference clone). |
| `--check-world=MODE` | `full` \| `fast` (alias `--fast-check`) \| `skip` (alias `--skip-check-world`). |
| `--force` | Run even if `(animal_name, pg_branch, git_commit_sha, test_profile)` was already marked `completed` in `perffarm_batches`. |
| `--no-template-reuse` | Re-initialize database from scratch for every test instead of caching template DBs. |
| `--skip-calibration` | Skip client-server `SELECT 1` / `repeat('0123456789', 100000)` calibration. |
| `--skip-setup` | Skip Step 0 machine discovery & Metrics DB setup (automatically set on commits `2..N` in multi-commit sweeps). |

### 2.4 `perffarm/provision_hosts.sh`
| Subcommand / Flag | Description |
| :--- | :--- |
| `provision` | Full provisioning: verify SSH -> install remote packages -> sync repo -> collect hardware/disk info -> run `setup_machine.sh` (default). |
| `verify-ssh` | Verify passwordless `ssh -o BatchMode=yes` connectivity to `SERVER_SSH_TARGET` and `CLIENT_SSH_TARGET`. |
| `sync-repo` | Sync local `pgbench4perffarm` repository to remote server/client workdirs via `rsync` (or `tar`-over-`ssh`). |
| `collect-info` | Run `collect_instance_info.py` on server (`--role=server`) and client (`--role=client`) and store JSON manifests in `$STATE_DIR/`. |
| `teardown` | Stop any running PostgreSQL clusters on the remote server workdir. |
| `status` | Show topology, SSH reachability, disk usage, and running `postgres` processes on server/client hosts. |
| `--config=PATH` | Path to `animal.conf` (default: `perffarm/conf/animal.conf`). |
| `--io-probe` | Include the 16 MB sequential write + `fsync` disk micro-probe during `collect-info`. |

### 2.5 `perffarm/upload_results.sh`
| Flag | Description |
| :--- | :--- |
| `--config=PATH` | Path to `animal.conf` (default: `perffarm/conf/animal.conf`). |
| `--test-dir=DIR` | Per-test result directory (`test_result_bundle.json`) for immediate `rest` webhook upload. |
| `--batch-dir=DIR` | Batch directory (`$RESULTS_DIR/batch_<id>_*`) to archive (`.tar.zst`/`.tar.gz`) and replicate to `REMOTE_METRICS_DB_CONNINFO`. |
| `--batch-id=ID` | Numeric `batch_id` in `perffarm_batches` (auto-detected from `$BATCH_DIR/batch_manifest.json` or `batch_<id>_*` dirname if omitted). |
| `--upload-level=LVL` | Override `UPLOAD_LEVEL` (`summary` \| `timeseries` \| `full-trace`). |
| `--psql-bin=PATH` | Path to `psql` binary. |

---

## 3. Critical `animal.conf` Configuration Variables

| Variable | Default (`animal.conf.example`) | Purpose |
| :--- | :--- | :--- |
| `ANIMAL_NAME` | `"lynx-x86-nvme"` | Unique identifier for this benchmarking machine/topology (`perffarm_animals.animal_name`). |
| `TOPOLOGY` | `"local"` | `"local"`, `"remote-server"`, `"remote-client"`, or `"both-remote"`. |
| `SERVER_SSH_TARGET` / `CLIENT_SSH_TARGET` | `""` | SSH targets (`user@host`) for remote topologies. |
| `WORKDIR` | `"/var/tmp/pgbench4perffarm_work"` | Workspace holding `src/`, `build/`, `install/`, `metrics_pg_install/`, `metrics_pgdata/`, `results/`, `archives/`, `state/`. |
| `PGDATA_DIR` / `PGWAL_DIR` | `"$WORKDIR/pgdata"` / `"$WORKDIR/pgwal"` | Server data & WAL directories; `collect_instance_info.py` probes system (`/`), data (`PGDATA_DIR`), and WAL (`PGWAL_DIR`) disks separately. |
| `USE_CCACHE` | `"auto"` | Enables `ccache` (`CCACHE_DIR="$WORKDIR/ccache"`) + `.perffarm_cfg_sig` incremental rebuilds across multi-commit sweeps. |
| `TEST_DURATION_SEQUENCE` | `""` (or `"60,60,60,300"`) | Default multi-duration sequence per prepared database. |
| `METRICS_DB_CONNINFO` | `"host=127.0.0.1 port=55433 dbname=perffarm_metrics"` | Primary PostgreSQL Metrics DB storing `pgbench_runs`, `perffarm_animals`, `perffarm_batches`, and `perffarm_test_runs`. |
| `UPLOAD_TARGETS` | `"metrics_db,archive"` | Comma-separated destinations: `metrics_db`, `archive`, `remote_metrics_db`, `rest`, `scp`. |
| `REMOTE_METRICS_DB_CONNINFO` | `""` | Optional central PostgreSQL Metrics DB for cross-animal replication (`upload_results.sh` acquires `pg_advisory_xact_lock(73733476)`, bumps sequences monotonically, and replicates `pgbench_*` + `perffarm_*` rows). |

---

## 4. Multi-Commit Sweep & Multi-Duration Execution Recipes

### 4.1 Historical Commit Sweep (`commits_master_24h_last_month.txt` or `90d`)
```bash
# Sweep 31 daily commits (~1 per 24h) on pgbench-c with 3x60s warmup + 1x300s steady-state
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
- **Safe Resume**: If interrupted mid-sweep, re-running the exact same command automatically skips already-completed commits in `< 1s` each prior to compiling PostgreSQL.

### 4.2 Syncing Local Animal Batches to a Central `remote_metrics_db`
```bash
# Ensure UPLOAD_TARGETS includes "remote_metrics_db" and REMOTE_METRICS_DB_CONNINFO is set in animal.conf:
for batch_dir in "$WORKDIR"/results/batch_*; do
  [ -d "$batch_dir" ] || continue
  ./perffarm/upload_results.sh --config=perffarm/conf/animal.conf --batch-dir="$batch_dir"
done
```

---

## 5. Longitudinal SQL Regression Analysis Cookbook (`perffarm_metrics`)

All queries below run against `METRICS_DB_CONNINFO` (or `REMOTE_METRICS_DB_CONNINFO`):

### 5.1 Inspect Latest Run or Compare Two Runs by ID / Label
```sql
-- Detailed scorecard for the most recent run (1. Run Info, 2. Overall Metrics, 3. Client Groups, 4. Scripts, 5. Recall & Aging, 6. Commands, 7. SLA Verifications)
SELECT * FROM pgbench_show_run();

-- Side-by-side comparison of two run_ids (returns: scope, metric, unit, base_val, cand_val, abs_delta, pct_change, verdict)
SELECT * FROM pgbench_compare_runs(101, 105);

-- Compare an arbitrary array of run_ids against the first element as baseline
SELECT * FROM pgbench_compare_runs(ARRAY[101, 102, 103, 104]::bigint[]);
```

### 5.2 Inspect Multi-Duration Step Variance (`60s` Steps `1..3` vs. `300s` Step `4`)
```sql
SELECT
    commit_short,
    git_commit_ts::date AS commit_date,
    MAX(CASE WHEN run_seq_idx = 1 THEN tps END) AS step1_60s_tps,
    MAX(CASE WHEN run_seq_idx = 2 THEN tps END) AS step2_60s_tps,
    MAX(CASE WHEN run_seq_idx = 3 THEN tps END) AS step3_60s_tps,
    MAX(CASE WHEN run_seq_idx = 4 THEN tps END) AS step4_300s_tps,
    MAX(CASE WHEN run_seq_idx = 4 THEN latency_p95_ms END) AS step4_300s_p95_ms,
    MAX(CASE WHEN run_seq_idx = 4 THEN latency_p99_ms END) AS step4_300s_p99_ms
FROM perffarm_commit_summary_v
WHERE test_name = 'pgbench-c'
GROUP BY commit_short, git_commit_ts
ORDER BY git_commit_ts DESC;
```

### 5.3 Detect All Automatic Regressions Across Commit History
`perffarm_regressions_v` partitions by `(animal_name, pg_branch, test_profile, test_name, run_seq_idx, target_duration_sec)` so a `60s` warmup step is only ever compared against the previous commit's `60s` warmup step at the same index, and a `300s` steady-state step is only compared against the previous commit's `300s` step:
```sql
SELECT animal_name, pg_branch, test_name, run_seq_idx, target_duration_sec,
       prev_commit_short, commit_short, git_commit_subject,
       prev_tps, tps, tps_pct_change,
       prev_latency_p95_ms, latency_p95_ms, latency_p95_pct_change, verdict
FROM perffarm_regressions_v
WHERE verdict IN ('REGRESSED', 'FAILED_VERIFICATION', 'IMPROVED')
ORDER BY batch_id DESC, run_seq_idx ASC;
```

### 5.4 Compare Any Two Git Commit SHAs Directly
```sql
-- Returns: animal_name, test_name, suite_name, metric, unit, sha_a_val, sha_b_val, abs_delta, pct_change, verdict
SELECT * FROM perffarm_compare_commits('2c10c2ce4d', 'dfd7ede529');

-- Scoped to a specific animal and test/suite
SELECT * FROM perffarm_compare_commits('2c10c2ce4d', 'dfd7ede529', 'lynx-x86-nvme', 'pgbench-c');
```

---

## 6. Perffarm Triage Table

| Failure Stage / Symptom | Root Cause | Deterministic Fix |
| :--- | :--- | :--- |
| `./perffarm/setup_machine.sh: line ...: PG_CONF_FILE: unbound variable` | Hand-wrote a partial `animal.conf` containing only a subset of variables instead of starting from `animal.conf.example` (`set -u` is active) | Always initialize `animal.conf` via `./perffarm/setup_machine.sh --config=perffarm/conf/animal.conf --init-config` (or `cp perffarm/conf/animal.conf.example ...`) and override specific settings via environment variables or edits. |
| `metrics_db` stops or loses `libpq.so` during a multi-commit sweep | `metrics_db` was started using mutable `$WORKDIR/install` binaries which get overwritten when older PG commits are built | Run `./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure` so `metrics_db` uses its own isolated `$WORKDIR/metrics_pg_install` binary tree (or set `METRICS_PG_CONFIG`). |
| `verify_test_run.py` returns `FAILED` on `4_sla_compliance` or `1_exit_code_zero` | A workload `--sla` threshold was breached (causing `pgbench` to exit `1`) | Inspect `checks.4_sla_compliance.sla_verifications` in `$TEST_DIR/test_result_bundle.json`. |
| `verify_test_run.py` returns `FAILED` on `5_server_log_clean` | PostgreSQL server emitted `PANIC:`, `FATAL:`, `TRAP:`, or cache/buffer leak warning | Inspect `checks.5_server_log_clean.severe_error_lines` in `$TEST_DIR/test_result_bundle.json` and `$LOG_DIR/postgres_*.log`. |
| `duplicate key value violates unique constraint` during `REMOTE_METRICS_DB_CONNINFO` sync | Sequence collision across multiple animals pushing to a shared central `metrics_db` | Always sync via `./perffarm/upload_results.sh` (which acquires `pg_advisory_xact_lock(73733476)`, bumps `pgbench_runs_run_id_seq` monotonically, and remaps `run_id` / `batch_id`). |
