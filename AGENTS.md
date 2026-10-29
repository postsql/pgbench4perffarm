<!--
  NOTE: The benchmark suites in pgbench4perffarm (pgbench-banking, pgbench-order,
  pgbench-brokerage, pgbench-analytics, pgbench-retail, pgbench-htap,
  pgbench-graph, pgbench-vec) are independent clean-room engineering workloads
  inspired by industry benchmark concepts (including TPC-B, TPC-C, TPC-E, TPC-H,
  TPC-DS, CH-benCHmark, LDBC, and VecBench). See src/bin/pgbench/workloads/README
  and BENCHMARK_INSPIRATION.md for full trademark disclaimers.
-->
# Agent Instructions for `pgbench4perffarm` (`//third_party/pgext/pgbench4perffarm`)

Welcome! This package (`//third_party/pgext/pgbench4perffarm`, upstream `postsql/pgbench4perffarm`) provides the next-generation **`pgbench`** client binary (`src/bin/pgbench/`), the **`contrib/pgbench`** server-side C extension (`contrib/pgbench/`), 8 enterprise & emerging workload suites (`src/bin/pgbench/workloads/`), the zero-dependency HTML/WASM telemetry viewer (`src/bin/pgbench/viewer/pgbench_viewer.py`), and the **`perffarm/`** continuous performance benchmarking & multi-commit historical sweep harness for **Jetski**, internal Google3 subagents (`DeepCoder`, `invoke_subagent`, `swarm`), Gemini CLI, Claude Code, Cursor, and Codex.

---

## 1. Progressive Disclosure Map for Jetski & Internal AI Agents

To conserve context window tokens and avoid command errors, load only the documentation file matching your active task:

| Task / Domain | Primary Agent Skill / Guide to Read First | Deep Reference |
| :--- | :--- | :--- |
| **Unified Skill (Part A `pgbench` + Part B `perffarm` + Google3 Blaze)** | [`skills/pgbench4perffarm/SKILL.md`](skills/pgbench4perffarm/SKILL.md) (installed at `~/.gemini/config/skills/pgbench4perffarm/SKILL.md`) | [`skills/README.md`](skills/README.md) |
| **Main `pgbench` & `contrib/pgbench` Usage** (Blaze & `PGXS` builds, `--config`, 8 workloads, `--group`, `--reservoir`, `--sla`, `--json-report`, `--log-db`) | [`doc/agent-guide-pgbench.md`](doc/agent-guide-pgbench.md) | [`skills/pgbench4perffarm/references/pgbench_main_usage_and_recipes.md`](skills/pgbench4perffarm/references/pgbench_main_usage_and_recipes.md) |
| **Perffarm Animal Setup, Runner & Multi-Commit Sweeps** (`setup_machine.sh`, `setup_metrics_db.sh`, `run_perffarm.sh`, `--commits-file`, `--durations`, `verify_test_run.py`, SQL regression views) | [`doc/agent-guide-perffarm.md`](doc/agent-guide-perffarm.md) | [`skills/pgbench4perffarm/references/perffarm_setup_and_runner.md`](skills/pgbench4perffarm/references/perffarm_setup_and_runner.md) |
| **Re-Creating or Porting the Wholesale Order-Entry (`pgbench-order` / `pgbench-c`) Workflow** (9-entity schema, `nurand`/`c_last`/`permute` formulas, all 5 transactions, `Model A` server-side `PL/pgSQL` stored procedures + `contrib/pgbench` vs. `Model B` client-driven SQL performance analysis, Spanner/MySQL/DuckDB/SQLite adaptations, Cypher/Document/KV non-SQL templates, and 8 consistency invariants) | [`doc/agent-guide-pgbench-order.md`](doc/agent-guide-pgbench-order.md) | [`skills/pgbench4perffarm/references/pgbench_order_workflow_and_porting_spec.md`](skills/pgbench4perffarm/references/pgbench_order_workflow_and_porting_spec.md) |
| **Authoring Custom `.sql` Scripts, `.pgbench.json` Configs, Client Expressions (61) or SQL Functions (51)** | [`skills/pgbench4perffarm/references/cli_and_functions_reference.md`](skills/pgbench4perffarm/references/cli_and_functions_reference.md) | [`doc/benchmark-language-guide.md`](doc/benchmark-language-guide.md) |

---

## 2. Google3 Package (`//third_party/pgext/pgbench4perffarm`) & Blaze Commands (`blaze-for-agents`)

In Google3, `//third_party/pgext/pgbench4perffarm` is the unified package containing both the server-side `contrib/pgbench` C extension and the `src/bin/pgbench` client benchmark driver, wired directly into Cloud SQL (`postgres12gce`..`postgresmastergce`) and AlloyDB (`postgres14alloydb`..`postgresmasteralloydb`, `postgres18alphadb`) server builds:

| Google3 Package | Contents | Key Blaze Targets |
| :--- | :--- | :--- |
| `//third_party/pgext/pgbench4perffarm` | Complete unified `pgbench4perffarm` package (`contrib/pgbench/` server extension for `>= 12.0`, `src/bin/pgbench/` client binary for `>= 16.0`, `src/common/pgbench_funcs.c`, `src/include/common/pgbench_funcs.h`, `perffarm/`, `doc/`, `skills/`, `AGENTS.md`). | `:postgres18gce/pgbench.so`, `:postgres18gce/pgbench`, `:data`, `:postgres18gce/regress`, `:postgres18gce/pgbench4perffarm_test`, `:workloads`, `:viewer`, `:perffarm`, `:skills`, `:all` |

### Building & Testing in Google3 via `blaze-for-agents`
Always use `/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents` (never bare `blaze` or `bazel`) from the CitC `google3/` root:

```bash
# 1. Run SQL regression tests (contrib/pgbench) and client/viewer/perffarm test suite on postgres18gce
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents test \
  //third_party/pgext/pgbench4perffarm:postgres18gce/regress \
  //third_party/pgext/pgbench4perffarm:postgres18gce/pgbench4perffarm_test

# 2. Build all targets across all supported Cloud SQL and AlloyDB PostgreSQL versions
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents build \
  //third_party/pgext/pgbench4perffarm:all
```

---

## 3. Standalone `PGXS` Build, Install & Test Commands (PostgreSQL 14–20devel)

When building outside Blaze in the standalone `PGXS` repository (e.g., `/usr/local/google/home/hannuk/pgbench4perffarm` with `PG_CONFIG=/usr/local/google/home/hannuk/pg20_install/bin/pg_config`; note that `//third_party/pgext/pgbench4perffarm` in Google3 builds exclusively via Blaze `BUILD` and never contains `Makefile` or `meson.build` files), pass `PG_CONFIG`:

```bash
PG_CONFIG="${PG_CONFIG:-/usr/local/google/home/hannuk/pg20_install/bin/pg_config}"

# 1. Build both contrib/pgbench (server C extension) and src/bin/pgbench (client binary)
make -j"$(nproc)" PG_CONFIG="$PG_CONFIG"

# 2. Install binary, C extension, workloads/, and viewer/ into the target PG installation
make install PG_CONFIG="$PG_CONFIG"

# 3. Run contrib/pgbench SQL regression suite against a running PostgreSQL cluster
make installcheck PG_CONFIG="$PG_CONFIG" PGPORT=5432

# 4. Run src/bin/pgbench Perl TAP integration suite
make tapcheck PG_CONFIG="$PG_CONFIG"
```

### Mandatory Post-Test Server Log Verification (C Extension Changes)
Whenever you modify C code in `contrib/pgbench/`, `src/common/pgbench_funcs.c`, or `src/bin/pgbench/`, inspect the PostgreSQL server log after running `make installcheck` or benchmark workloads to verify zero occurrences of:
- `WARNING: cache reference leak` / `WARNING: TupleDesc reference leak`
- `WARNING: buffer refcount leak` / `WARNING: Snapshot reference leak`
- `TRAP: FailedAssertion` / `Segmentation fault` / `PANIC:`

---

## 4. Non-Negotiable Operational Rules for Jetski & Internal Agents

1. **Google3 CitC Search Rule (`code_search` Mandatory, No `find`/`grep` in `google3/`)**:
   - **NEVER** execute shell `find`, `grep`, `rg`, or `ag` inside any `google3/` or CitC (`/google/src/cloud/...`) directory. Always use the `code_search` tool (`f:`, `c:`, `s:`, `trait:dir`). Remind any delegated subagents (`DeepCoder`, `invoke_subagent`) of this rule.
2. **CitC Workspace Hygiene (Never Write Perffarm `$WORKDIR` or `--json-report` into CitC)**:
   - CitC workspaces (`/google/src/cloud/...`) are world-readable and permanently retain all writes in `.snapshot/`. Always direct Perffarm `WORKDIR` (e.g. `/var/tmp/pgbench4perffarm_work` or `/usr/local/google/home/$USER/perffarm_work`), `metrics_pgdata`, and `--json-report=/tmp/...` to local disk outside CitC.
3. **Google3 `//third_party/pgext/` Packaging & Compliance (No `Makefile`/`meson.build`, Mandatory `LICENSE`)**:
   - Never upload or commit `Makefile` or `meson.build` files into `//third_party/pgext/` in Google3 (keep them only in standalone `PGXS` repositories and exclude `"Makefile"`, `"**/Makefile"`, `"meson.build"`, `"**/meson.build"` in `copy.bara.sky`).
   - Always include a top-level `LICENSE` file containing the full PostgreSQL License text and declare `default_applicable_licenses = [":license"]` with `license(name = "license", package_name = "pgbench4perffarm")` in `BUILD`.
4. **Prefer `--config=<file>.pgbench.json` over manual multi-step CLI flags**:
   - Initialize: `pgbench --config=src/bin/pgbench/workloads/pgbench-c/pgbench_c.pgbench.json -i -s 2 "$DB"`
   - Execute: `pgbench --config=src/bin/pgbench/workloads/pgbench-c/pgbench_c.pgbench.json -s 2 -c 8 -j 4 -T 30 --json-report=/tmp/out.json "$DB"`
5. **NEVER combine `-i` with `-f`**:
   - `-i` is strictly for built-in default initialization (`pgbench -i -s 10`) or `--config=<file>.pgbench.json -i` (combining `-i` with `-f` fails with `pgbench: error: some of the specified options cannot be used in initialization (-i) mode`). When invoking manual `*_schema.sql` or `*_init.sql` files directly, always use `pgbench -n -t 1 -f <script>.sql "$DB"` **without `-i`**.
6. **Understand `--config` vs. `--group` vs. `-c`/`-j`/`-f` precedence**:
   - When `--config=<file>.pgbench.json` defines `groups[]`, passing `-c N` or `-j M` on the CLI **proportionally scales** the configured group client/thread counts while preserving group ratios (`pgbench.c:1374-1447`).
   - Passing `-f`, `-b`, or `--group` on the CLI clears all scripts and groups loaded from `--config` (`clearConfigScriptsAndGroups()`). When `--group` is passed directly on the CLI with multiple groups, `nclients` and `nthreads` are computed as the sum of each group's `clients`/`threads` (overriding top-level `-c`/`-j`).
7. **NEVER run `\phase_barrier` scripts (`snb_bi.pgbench.json`) with `-T <seconds>`**:
   - Always use bounded transaction counts (`-t <N>`) so all clients enter the exact same number of barrier phases.
8. **Always use `\reservoir_sample` (never `\reservoir_pop`)**:
   - Syntax: `\reservoir_sample <reservoir_name> <var_name> [<default_expr>]` with `--reservoir=<name>:<capacity>`.
9. **Check `--json-report` before treating Exit Code `1` as a crash**:
   - `pgbench` exits with `0` (all SLAs passed), `1` (**either** an `--sla` threshold failed with `$.sla_all_passed == false` in `--json-report`, **or** a fatal CLI/SQL error occurred), or `2` (client aborted mid-run). Always inspect `$.sla_all_passed` and `$.sla_verifications` in the JSON report when exit code is `1`.
10. **Keep `perffarm_metrics` isolated from SUT rebuilds in `perffarm/`**:
    - Always bootstrap the Metrics DB via `./perffarm/setup_metrics_db.sh --config=perffarm/conf/animal.conf --ensure`, which maintains an isolated `$WORKDIR/metrics_pg_install` binary tree so rebuilding `$WORKDIR/install` during multi-commit sweeps never disrupts the Metrics DB.
11. **Use `--durations=60,60,60,300` for warmup + steady-state sequences on a single prepared DB**:
    - In `perffarm/run_perffarm.sh`, `--durations=60,60,60,300` initializes each suite once and executes 4 sequential timed steps against that same database, recording `sequence_run_index` (`1..4`), `target_duration_sec`, and `prepared_db_reused` (`false` on step 1, `true` on steps 2–4) in `perffarm_test_runs.verification_details`.
12. **Compare matching sequence steps & durations in SQL regression views**:
    - `perffarm_commit_summary_v`, `perffarm_regressions_v`, and `perffarm_compare_commits(sha_a, sha_b)` partition by `(animal_name, pg_branch, test_profile, test_name, run_seq_idx, target_duration_sec)` so `60s` warmup runs are never mixed with `300s` steady-state runs.
13. **Legal & Trademark Guardrails**:
    - Per `src/bin/pgbench/workloads/README`, do **not** call the benchmark workloads by trademarked names in filenames, titles, headings, prose, or scripts; mention inspirations only in an opening file comment and use our preferred benchmark names corresponding to the benchmark scripts: **`pgbench-banking`** (`pgbench-b`), **`pgbench-order`** (`pgbench-c`, wholesale order-entry), **`pgbench-brokerage`** (`pgbench-e`), **`pgbench-analytics`** (`pgbench-h`), **`pgbench-retail`** (`pgbench-ds`), **`pgbench-htap`**, **`pgbench-graph`**, and **`pgbench-vec`**. Never claim official benchmark certification or report trademarked council metrics.
