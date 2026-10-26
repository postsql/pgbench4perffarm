# Agent Instructions for `postsql/pgbench4perffarm`

Welcome! This repository (`postsql/pgbench4perffarm`) packages the next-generation **`pgbench`** client binary (`src/bin/pgbench/`), the **`contrib/pgbench`** server-side C extension (`contrib/pgbench/`), 8 enterprise & emerging workload suites (`src/bin/pgbench/workloads/`), the zero-dependency HTML/WASM telemetry viewer (`src/bin/pgbench/viewer/pgbench_viewer.py`), and the **`perffarm/`** continuous performance benchmarking & multi-commit historical sweep harness.

---

## 1. Progressive Disclosure Map for AI Agents

To conserve context window tokens and avoid command errors, load only the documentation file matching your active task:

| Task / Domain | Primary Agent Skill / Guide to Read First | Deep Reference |
| :--- | :--- | :--- |
| **Unified Skill (Part A `pgbench` + Part B `perffarm`)** | [`skills/pgbench4perffarm/SKILL.md`](skills/pgbench4perffarm/SKILL.md) | [`skills/README.md`](skills/README.md) |
| **Main `pgbench` & `contrib/pgbench` Usage** (Building, `--config`, 8 workloads, `--group`, `--reservoir`, `--sla`, `--json-report`, `--log-db`) | [`doc/agent-guide-pgbench.md`](doc/agent-guide-pgbench.md) | [`skills/pgbench4perffarm/references/pgbench_main_usage_and_recipes.md`](skills/pgbench4perffarm/references/pgbench_main_usage_and_recipes.md) |
| **Perffarm Animal Setup, Runner & Multi-Commit Sweeps** (`setup_machine.sh`, `setup_metrics_db.sh`, `run_perffarm.sh`, `--commits-file`, `--durations`, `verify_test_run.py`, SQL regression views) | [`doc/agent-guide-perffarm.md`](doc/agent-guide-perffarm.md) | [`skills/pgbench4perffarm/references/perffarm_setup_and_runner.md`](skills/pgbench4perffarm/references/perffarm_setup_and_runner.md) |
| **Authoring Custom `.sql` Scripts, `.pgbench.json` Configs, Client Expressions (61) or SQL Functions (51)** | [`skills/pgbench4perffarm/references/cli_and_functions_reference.md`](skills/pgbench4perffarm/references/cli_and_functions_reference.md) | [`doc/benchmark-language-guide.md`](doc/benchmark-language-guide.md) |

---

## 2. Build, Install & Test Commands (`PGXS` Across PostgreSQL 14–20devel)

Always pass `PG_CONFIG=/path/to/pg_config` when targeting a specific PostgreSQL installation:

```bash
# 1. Build both contrib/pgbench (server C extension) and src/bin/pgbench (client binary)
make -j"$(nproc)" PG_CONFIG=/path/to/pg_config

# 2. Install binary, C extension, workloads/, and viewer/ into the target PG installation
make install PG_CONFIG=/path/to/pg_config

# 3. Run contrib/pgbench SQL regression suite against a running PostgreSQL cluster
make installcheck PG_CONFIG=/path/to/pg_config PGPORT=5432

# 4. Run src/bin/pgbench Perl TAP integration suite
make tapcheck PG_CONFIG=/path/to/pg_config
```

### Mandatory Post-Test Server Log Verification (C Extension Changes)
Whenever you modify C code in `contrib/pgbench/`, `src/common/pgbench_funcs.c`, or `src/bin/pgbench/`, inspect the PostgreSQL server log after running `make installcheck` or benchmark workloads to verify zero occurrences of:
- `WARNING: cache reference leak` / `WARNING: TupleDesc reference leak`
- `WARNING: buffer refcount leak` / `WARNING: Snapshot reference leak`
- `TRAP: FailedAssertion` / `Segmentation fault` / `PANIC:`

---

## 3. Top-10 Non-Negotiable Operational Rules for Agents

1. **Prefer `--config=<file>.pgbench.json` over manual multi-step CLI flags**:
   - Initialize: `pgbench --config=src/bin/pgbench/workloads/pgbench-c/pgbench_c.pgbench.json -i -s 2 "$DB"`
   - Execute: `pgbench --config=src/bin/pgbench/workloads/pgbench-c/pgbench_c.pgbench.json -s 2 -c 8 -j 4 -T 30 --json-report=/tmp/out.json "$DB"`
2. **NEVER combine `-i` with `-f`**:
   - `-i` is strictly for built-in TPC-B (`pgbench -i -s 10`) or `--config=<file>.pgbench.json -i` (combining `-i` with `-f` fails with `pgbench: error: some of the specified options cannot be used in initialization (-i) mode`). When invoking manual `*_schema.sql` or `*_init.sql` files directly, always use `pgbench -n -t 1 -f <script>.sql "$DB"` **without `-i`**.
3. **Understand `--config` vs. `--group` vs. `-c`/`-j`/`-f` precedence**:
   - When `--config=<file>.pgbench.json` defines `groups[]`, passing `-c N` or `-j M` on the CLI **proportionally scales** the configured group client/thread counts while preserving group ratios (`pgbench.c:1374-1447`).
   - Passing `-f`, `-b`, or `--group` on the CLI clears all scripts and groups loaded from `--config` (`clearConfigScriptsAndGroups()`). When `--group` is passed directly on the CLI with multiple groups, `nclients` and `nthreads` are computed as the sum of each group's `clients`/`threads` (overriding top-level `-c`/`-j`).
4. **NEVER run `\phase_barrier` scripts (`snb_bi.pgbench.json`) with `-T <seconds>`**:
   - Always use bounded transaction counts (`-t <N>`) so all clients enter the exact same number of barrier phases.
5. **Always use `\reservoir_sample` (never `\reservoir_pop`)**:
   - Syntax: `\reservoir_sample <reservoir_name> <var_name> [<default_expr>]` with `--reservoir=<name>:<capacity>`.
6. **Check `--json-report` before treating Exit Code `1` as a crash**:
   - `pgbench` exits with `0` (all SLAs passed), `1` (**either** an `--sla` threshold failed with `$.sla_all_passed == false` in `--json-report`, **or** a fatal CLI/SQL error occurred), or `2` (client aborted mid-run). Always inspect `$.sla_all_passed` and `$.sla_verifications` in the JSON report when exit code is `1`.
7. **Keep `perffarm_metrics` isolated from SUT rebuilds in `perffarm/`**:
   - Always bootstrap the Metrics DB via `./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure`, which maintains an isolated `$WORKDIR/metrics_pg_install` binary tree so rebuilding `$WORKDIR/install` during multi-commit sweeps never disrupts the Metrics DB.
8. **Use `--durations=60,60,60,300` for warmup + steady-state sequences on a single prepared DB**:
   - In `perffarm/run_perffarm.sh`, `--durations=60,60,60,300` initializes each suite once and executes 4 sequential timed steps against that same database, recording `sequence_run_index` (`1..4`), `target_duration_sec`, and `prepared_db_reused` (`false` on step 1, `true` on steps 2–4) in `perffarm_test_runs.verification_details`.
9. **Compare matching sequence steps & durations in SQL regression views**:
   - `perffarm_commit_summary_v`, `perffarm_regressions_v`, and `perffarm_compare_commits(sha_a, sha_b)` partition by `(animal_name, pg_branch, test_profile, test_name, run_seq_idx, target_duration_sec)` so `60s` warmup runs are never mixed with `300s` steady-state runs.
10. **Legal & Trademark Guardrails**:
    - Always describe workloads as *"TPC-B-inspired (`pgbench-b`)"*, *"TPC-C-inspired (`pgbench-c`)"*, *"TPC-E-inspired (`pgbench-e`)"*, *"TPC-H-inspired (`pgbench-h`)"*, *"TPC-DS-inspired (`pgbench-ds`)"*, *"LDBC-inspired (`pgbench-graph`)"*, or *"VecBench-inspired (`pgbench-vec`)"*. Never claim official benchmark certification.
