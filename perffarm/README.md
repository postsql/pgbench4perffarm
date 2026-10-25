# `perffarm/` — PostgreSQL Perffarm Animal Setup & Continuous Runner

This directory contains the self-contained configuration templates, machine setup scripts, per-instance hardware/storage inventory collector, client-server network/IPC calibration tool, Metrics Database schema, and automated execution harness for running **`pgbench4perffarm`** as a PostgreSQL Performance Farm ("perffarm") animal.

For the full architectural and operational reference, see **[`doc/perffarm-guide.md`](../doc/perffarm-guide.md)**.

---

## Directory Layout

| Path | Purpose |
| :--- | :--- |
| [`conf/animal.conf.example`](conf/animal.conf.example) | Template animal configuration (`cp conf/animal.conf.example conf/animal.conf`): animal identity, deployment topology (`local`, `remote-server`, `remote-client`, `both-remote`), workspace & separate system/data/WAL storage paths, build flags, GUC sizing, Metrics DB, calibration, and upload targets. |
| [`conf/postgresql.perffarm.conf.in`](conf/postgresql.perffarm.conf.in) | Deterministic PostgreSQL server configuration template parameterized by [`generate_pgconf.py`](generate_pgconf.py). |
| [`conf/test_matrix.json`](conf/test_matrix.json) | Declarative test catalog and sizing presets (`smoke`, `standard`, `nightly`) across all 26 built-in, OLTP (`pgbench-b/c/e`), DSS/OLAP (`pgbench-h/ds`), HTAP (`pgbench-htap`), Graph (`SQL`, `SQL_UD`, `PGQ`), and Vector (`pgbench-vec`) workloads. |
| [`setup_machine.sh`](setup_machine.sh) | One-time (and idempotent) machine setup: verifies compiler/dependencies (`--install-deps`), optionally applies Linux kernel & CPU governor tuning (`--tune-os`), collects per-instance CPU/RAM/disk profiles, and generates `postgresql.animal.conf`. |
| [`provision_hosts.sh`](provision_hosts.sh) | Helper to provision, verify SSH connectivity, sync `pgbench4perffarm`, collect instance profiles, or tear down external / Cloud VM instances (`local`, `ssh`, `gcp`, `aws`, `custom`). |
| [`collect_instance_info.py`](collect_instance_info.py) | Inspects local or remote (`--ssh-target`) server and client instances, recording CPU count/model/governor, RAM/HugePages, and separate disk metrics for **system disk (`/`)**, **data disk (`PGDATA_DIR`)**, and **WAL disk (`PGWAL_DIR`)**. |
| [`generate_pgconf.py`](generate_pgconf.py) | Generates a deterministic `postgresql.animal.conf` from `hardware_profile.json` or sizing presets (`small-dev`, `medium-vm`, `large-baremetal`) with PostgreSQL 14–20 GUC version filtering. |
| [`setup_metrics_db.sh`](setup_metrics_db.sh) | Provisions and starts a local persistent Metrics PostgreSQL cluster (or connects to an external `METRICS_DB_CONNINFO`), applies [`sql/metrics_db_schema.sql`](sql/metrics_db_schema.sql), and registers the animal in `perffarm_animals`. |
| [`sql/metrics_db_schema.sql`](sql/metrics_db_schema.sql) | Idempotent DDL installing the 9 `pgbench --log-db` tables/views plus `perffarm_animals`, `perffarm_batches`, `perffarm_test_runs`, `perffarm_commit_summary_v`, `perffarm_regressions_v`, and `perffarm_compare_commits(sha_a, sha_b)`. |
| [`calibrate_baseline.py`](calibrate_baseline.py) | Measures client-to-server baseline round-trip latency (`SELECT 1;`) and 1 MB bulk payload throughput (`SELECT repeat('0123456789', 100000);`) before benchmark runs. |
| [`collect_pg_stats.py`](collect_pg_stats.py) | Captures pre-test and post-test `pg_stat_database`, `pg_stat_bgwriter`, `pg_stat_checkpointer`, `pg_stat_wal`, `pg_stat_io`, `pg_stat_statements`, and Linux `/proc` CPU/VM counters on both server and client instances, and computes exact benchmark deltas. |
| [`verify_test_run.py`](verify_test_run.py) | Enforces the 5-point test verification contract (`exit_code == 0`, `transactions > 0` & `failed_transactions == 0`, `100%` `\validate_result`, `--sla` pass, and clean `postgres.log`) and records the run in `perffarm_test_runs`. |
| [`upload_results.sh`](upload_results.sh) | Packages and uploads per-test and per-batch telemetry to the Metrics DB, `.tar.zst` archives, remote PostgreSQL Metrics DBs, REST endpoints, or SCP targets. |
| [`run_perffarm.sh`](run_perffarm.sh) | Main end-to-end orchestrator for regular Perffarm runs. |
| [`systemd/`](systemd/) | `pgbench-perffarm.service` and `pgbench-perffarm.timer` units for automated scheduling. |

---

## Supported Deployment Topologies (Local vs. External / Cloud VMs)

Both the PostgreSQL server and the `pgbench` client can run on the local machine or on newly provisioned external machines / Cloud VMs:

| `TOPOLOGY` Setting | PostgreSQL Server Instance | `pgbench` Client Instance | Typical Use Case |
| :--- | :--- | :--- | :--- |
| **`local`** *(default)* | Local host (`127.0.0.1`) | Local host (`127.0.0.1`) | Single bare-metal server or developer workstation/VM. |
| **`remote-server`** | Remote VM (`SERVER_SSH_TARGET`) | Local host | Local controller driving a dedicated remote/Cloud PostgreSQL VM. |
| **`remote-client`** | Local host | Remote VM (`CLIENT_SSH_TARGET`) | Isolating load-generator CPU overhead on an external client VM. |
| **`both-remote`** | Remote VM A (`SERVER_SSH_TARGET`) | Remote VM B (`CLIENT_SSH_TARGET`) | Ephemeral or dedicated Cloud VMs for both server and client, orchestrated from a lightweight controller. |

For every topology, `perffarm/` automatically collects and records:
1. **Per-Instance Hardware & Disk Inventory (`server_instance_info`, `client_instance_info`)**:
   - Logical & physical CPU count, CPU model, frequency, scaling governor, and NUMA nodes.
   - Total & available RAM, swap, HugePages, and Transparent HugePage mode.
   - Separate block-device, filesystem, mount-option, capacity, media-type (`NVMe`, `SSD`, `HDD`, `tmpfs`), and sequential-write/fsync micro-probe metrics for:
     - **System disk (`/`)**
     - **Data disk (`PGDATA_DIR`)**
     - **WAL disk (`PGWAL_DIR`)** (including whether `PGDATA_DIR` and `PGWAL_DIR` reside on dedicated underlying block devices).
2. **Client-Server Baseline Network/IPC Calibration (`baseline_calibration`)**:
   - **`SELECT 1;`**: Client-to-server round-trip latency (`avg`, `min`, `p50`, `p90`, `p95`, `p99`, `max` in milliseconds and microseconds) and single-connection QPS.
   - **`SELECT repeat('0123456789', 100000);`**: Client-to-server 1,000,000-byte (1 MB) result transfer latency (`avg`, `p50`, `p95`, `p99` ms), rows/sec, and effective transfer bandwidth (`throughput_mb_per_sec`, `throughput_mib_per_sec`, `throughput_gbps`).

---

## Step 1: One-Time Setting Up the Perffarm Animal

### 1.1 Create & Customize `perffarm/conf/animal.conf`

```bash
cp perffarm/conf/animal.conf.example perffarm/conf/animal.conf
```

Key settings in `perffarm/conf/animal.conf`:
- `ANIMAL_NAME`: Unique identifier for this animal (e.g., `lynx-x86-nvme`).
- `TOPOLOGY`: `local`, `remote-server`, `remote-client`, or `both-remote`.
- `PGDATA_DIR` & `PGWAL_DIR`: Mount paths for the PostgreSQL data directory and WAL directory (point these to separate NVMe/SSD mount points when available).
- `PG_GIT_URL`, `PG_BRANCHES`, `CFLAGS`, `CONFIGURE_FLAGS`: Server source repository and release build flags (`-O2 -g -fno-omit-frame-pointer`, never `--enable-cassert`).
- `PG_SIZING_PRESET`: `auto` (hardware-derived), `small-dev`, `medium-vm`, or `large-baremetal`.
- `METRICS_DB_MODE`: `local` (provisions a dedicated persistent Postgres cluster on `METRICS_DB_PORT`, default `55433`) or `external` (`METRICS_DB_CONNINFO`).
- `TEST_PROFILE`: `smoke`, `standard`, or `nightly`.
- `METRICS_LEVEL` & `UPLOAD_LEVEL`: `summary`, `timeseries` (default), or `full-trace`.

### 1.2 Run Machine Setup & Hardware Discovery (`setup_machine.sh`)

```bash
# Unprivileged setup (creates workspace, checks tools, probes disks, writes postgresql.animal.conf):
./perffarm/setup_machine.sh --config=perffarm/conf/animal.conf

# Optional on dedicated root hosts (installs apt/dnf build deps and tunes sysctl / CPU governor):
./perffarm/setup_machine.sh --config=perffarm/conf/animal.conf --install-deps --tune-os
```

### 1.3 Provision the Metrics Database (`setup_metrics_db.sh`)

```bash
./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure
./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --status
```

---

## Step 2: Regular Perffarm Run (`run_perffarm.sh`)

### 2.1 Full Automated Run (Checkout -> Build -> `check-world` -> Calibrate -> Benchmark -> Verify -> Upload)

```bash
./perffarm/run_perffarm.sh --config=perffarm/conf/animal.conf
```

### 2.2 Common CLI Overrides for Development & CI

```bash
# Fast smoke run across selected suites using an existing PostgreSQL installation:
./perffarm/run_perffarm.sh \
    --config=perffarm/conf/animal.conf \
    --prebuilt-pg-config=/path/to/pg_config \
    --profile=smoke \
    --tests=tpcb-like,pgbench-b,pgbench-c,pgbench-htap,pgbench-graph-lsqb,pgbench-vec-hybrid \
    --fast-check \
    --force

# Run against a remote PostgreSQL server VM over SSH:
./perffarm/run_perffarm.sh \
    --config=perffarm/conf/animal.conf \
    --topology=remote-server \
    --server-ssh=ubuntu@10.128.0.10 \
    --profile=standard

# Run a historical multi-commit series (e.g., 24-hour interval checkouts on master):
./perffarm/run_perffarm.sh \
    --config=perffarm/conf/animal.conf \
    --branch=master \
    --commits-file=commits_24h_last_month.txt \
    --ci-status=github_success \
    --tests=pgbench-c \
    --profile=standard
```

---

## Step 3: Querying Results & Launching the Web Telemetry Viewer

### 3.1 Inspect Runs & Longitudinal Regressions in SQL

```bash
psql -d "host=127.0.0.1 port=55433 dbname=perffarm_metrics" <<'SQL'
-- 1. Summary of all test runs with server/client CPU/RAM/disk & SELECT 1 / 1MB calibration:
SELECT batch_id, animal_name, topology, commit_short, test_name, verification_status,
       tps, latency_p95_ms, select1_avg_ms, repeat1mb_mb_per_sec,
       server_cpus, server_ram_gb, server_data_disk_media
FROM perffarm_commit_summary_v;

-- 2. Commit-over-commit regression detection view:
SELECT animal_name, pg_branch, test_name, prev_commit_short, commit_short,
       prev_tps, tps, tps_pct_change, latency_p95_pct_change, verdict
FROM perffarm_regressions_v;

-- 3. Compare two commits side-by-side across all suites:
SELECT * FROM perffarm_compare_commits('a1b2c3d4', 'e5f6g7h8');
SQL
```

### 3.2 Launch the WebAssembly Telemetry Viewer Against the Metrics DB

```bash
python3 viewer/pgbench_viewer.py \
    --log-db="host=127.0.0.1 port=55433 dbname=perffarm_metrics" \
    --port=8094
```
