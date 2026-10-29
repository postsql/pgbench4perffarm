# Complete CLI Flags, Backslash Meta-Commands, Client & `contrib/pgbench` SQL Functions Reference

This reference lists all `pgbench` command-line options (`pgbench.c`), `--sla` grammar (`stats.c`), 39 backslash meta-commands (`commands.c`), 61 client-side expression functions (`exprparse.y`), 51 `contrib/pgbench` server C/SQL functions in `SCHEMA pgbench` (`pgbench--1.0.sql`), and `--log-db` / Perffarm SQL set-returning functions (`metrics_db_schema.sql`).

---

## 1. Key `pgbench` Command-Line Options (`src/bin/pgbench/pgbench.c`)

| Option | Argument | Description |
| :--- | :--- | :--- |
| `--config` | `FILE` | Load a declarative `.pgbench.json` (JSON/JSONC) workload configuration (`init_steps`, `groups`, `reservoirs`, `sla`, `variables`, `duration_sec`, `transactions`). Combine with `-i` to execute `init_steps[]`. Passing `-c N` or `-j M` with `--config` proportionally scales configured group client/thread counts. Also accepts a prior `--json-report` file for direct replay. |
| `--dump-config` | `FILE\|-` | Serialize the merged configuration (after applying `--config` and CLI overrides) as JSON to `FILE` (or `-` for stdout) and exit `0`. |
| `--group` | `SPEC` | Define an independent client pool: `name=NAME,clients=N[,threads=M][,sequential=true\|false][,rate=R],file=PATH[@WEIGHT][,builtin=NAME[@WEIGHT]]...`. Repeatable. Passing `--group`, `-f`, or `-b` on the CLI clears any scripts/groups loaded from `--config`. |
| `--reservoir` | `NAME:SIZE` | Create a shared lock-free atomic reservoir of capacity `SIZE` across all client threads. Repeatable. |
| `--sla` | `RULE[,RULE...]` | SLA assertion (`[<target>:][<named_metric>:]<metric><op><threshold>`). Repeatable (`--sla="..." --sla="..."`) or comma-separated. |
| `--json-report` | `FILE` | Write structured JSON telemetry report to `FILE`. |
| `--log-db` | `CONNINFO\|self` | Stream benchmark telemetry (`pgbench_runs` + 8 child/stream tables) into PostgreSQL (`self` logs into the target DB; or pass a libpq conninfo string). |
| `--log-db-unlogged` | `stream\|all\|off` | Table persistence mode (`stream` = Tier A logged, Tier B/C unlogged; `all` = all 9 tables unlogged; `off` = all 9 tables logged). |
| `--log-db-mode` | `stream\|deferred` | `stream` flushes live `COPY` batches during the run; `deferred` spools to local temp files and bulk-loads via `COPY` after the timer stops. |
| `--log-db-schema` | `SCHEMA` | Target PostgreSQL schema for `--log-db` tables, views, and SRFs (default `public`). |
| `--log-db-index` | *(none)* | Create secondary B-tree indexes on Tier B/C stream tables (`pgbench_progress_log`, `pgbench_agg_log`, `pgbench_tx_log`) after `COPY` completes. |
| `--run-label` | `LABEL` | Human/agent tag stored in `pgbench_runs.run_label` and `--json-report`. |
| `--dataset` | `SPEC` | Memory-map (`mmap`) a dataset file (`fvecs`, `bvecs`, `ivecs`, `csv`/`psv`/`pipe`, `tsv`): `<name>[:<format>][:header]=<path>` or `<name>:<path>[:<format>][:header]`. |
| `--vector-dataset` | `SPEC` | Alias for `--dataset=SPEC` (memory-maps `.fvecs`, `.bvecs`, `.ivecs`, `.csv`, or `.tsv` datasets). |
| `--copy-dataset` | `SPEC` | Stream an `mmap` dataset into a table via parallel `COPY FROM STDIN` during `-i`: `<dataset>:<table>[(col1,...)]`. |
| `--vector-base` | `SPEC` | Shorthand `[<table>:][<id_offset>:]<path.fvecs>`: registers dataset `"base"` (`fvecs`) with primary key `<id_offset>` (default `1`) and registers a `--copy-dataset` target for `<table>` (default `vecbench_items`). |
| `--ground-truth-table` | `SPEC` | Shorthand `[<table>:][<id_offset>:]<path.ivecs>`: registers dataset `"ground_truth"` (`ivecs`) with query ID `<id_offset>` (default `1`) and registers a `--copy-dataset` target for `<table>` (default `vecbench_ground_truth`). |
| `--replay` | `FILE` | Replay transaction arrival timestamps from `FILE`. |
| `--replay-speed`, `--tcr`| `FACTOR` | Scale trace replay speed by `FACTOR` (`2.0` = 2× faster). |
| `--ontime-threshold` | `MS` | Maximum schedule lag in ms counted as on-time in `ontime_pct` (default `50.0`). |
| `--max-concurrent-sessions`| `N` | Global cap on dynamic `\session_connect` connections. |

---

## 2. `--sla` Rule Grammar (`--sla="[<target>:][<named_metric>:]<metric><op><threshold>"`)

| Field | Supported Values | Examples |
| :--- | :--- | :--- |
| `<target>` | `total` / `all` / `*` (default when omitted), `<group_name>`, `<script_name>` (or script basename without `.sql`) | `total:p95<=50ms`, `oltp:p99<=25ms`, `pgbench_order_new_order:p95<=40ms` |
| `<named_metric>` | Any named recall stream recorded via `\record_recall [name=]<metric>` (or `schedule_lag` when combined with `:p<X>`) | `r_unfiltered:recall>=0.95`, `r_unfiltered:recall:p5>=0.80`, `schedule_lag:p99<=10ms` |
| `<metric>` | `p<X>` (`p50`, `p90`, `p95`, `p99`, `p99.9`), `avg` / `latency_avg`, `schedule_lag:p<X>` / `schedule_lag_p<X>`, `ontime_pct`, `recall`, `recall_p<X>` / `recall:p<X>`, `tps` / `qps`, `error_rate`, `validation_failures`, `validation_pct` | `p95<=50ms`, `ontime_pct>=95%`, `recall>=0.90`, `tps>=500`, `error_rate<=0%`, `validation_failures<=0` |
| `<op>` | `<=`, `>=` | `<=`, `>=` |
| `<threshold>` | Number with optional unit (`us`, `ms`, `s`, `%`) | `100ms`, `1.5s`, `0.95`, `99.9%`, `0` |

---

## 3. Backslash Meta-Commands (39 Commands in `script.h` / `commands.c`)

| Category | Command Syntax | Description |
| :--- | :--- | :--- |
| **Variables & Result Binding (7)** | `\set <var>[<idx>] <expr>` | Evaluate client expression `<expr>` and assign typed `PgBenchValue` to `:<var>` (or element `<idx>`). |
| | `\setshell <var> <cmd> [<arg>...]` | Execute shell command and assign integer stdout to `:<var>`. |
| | `\shell <cmd> [<arg>...]` | Execute shell command without capturing output. |
| | `\gset [<prefix>]` | Execute preceding SQL (1 row) and bind columns (auto-parsing `{...}` arrays, `[...]` vectors, `\x...` bytea). |
| | `\aset [<prefix>]` | Bind last row of each query in `\;` compound statement, or bind **all rows** into `PGBT_ARRAY` (`<prefix><col>` and `<prefix>_<col>`) when `<prefix>` is given. |
| | `\gset_col <var> [<col>]` | Capture **all rows** of a single column (name or 0-based index, default `0`) into 1D `PGBT_ARRAY` `:<var>`. |
| | `\gset_meta [<prefix>]` | Capture `:<prefix>_status` (`"OK"`/`"ERROR"`), `:<prefix>_sqlstate`, and `:<prefix>_nrows` (default prefix `meta`). |
| **Control Flow & Loops (8)** | `\if <expr>` / `\elif <expr>` / `\else` / `\endif` | Conditional branching (uses SQL `=` and `<>` / `!=`). |
| | `\for <var> <start> .. <end> [BY <step>]` ... `\endfor` | Bounded integer loop (`\for` + `\endfor`). |
| | `\while <expr>` ... `\endwhile` | Condition-driven loop (`\while` + `\endwhile`). |
| **Exception Handling (4)** | `\try` ... `\catch ['SQLSTATE']` ... `\endtry` | Catch SQL errors (`\try` + `\catch` + `\endtry`; populates `:sqlstate`, `:error_message` and increments `expected_rollbacks` instead of `transactions_failed`). |
| | `\expect_error ['SQLSTATE']` | Single-statement prefix or suffix shorthand for expected SQL errors. |
| **Pacing & Think Time (3)** | `\sleep <expr> [us\|ms\|s]` | Non-blocking pause included in transaction cycle time. |
| | `\keying_time <expr> [us\|ms\|s]` | Simulated terminal keying delay before transaction (subtracted from DB response time). |
| | `\think_time <expr> [us\|ms\|s]` | Simulated user think time after transaction (subtracted from DB response time). |
| **Barriers, Reservoirs & Watermarks (7)** | `\phase_barrier [<phase_name>]` | Synchronize all active clients across all threads at a phase boundary (requires `-t <N>`). |
| | `\reservoir_push <name> <expr>` | Push integer ID (or `PGBT_ARRAY` of IDs) into lock-free reservoir `<name>`. |
| | `\reservoir_sample <name> <var> [<default>]` | Sample live ID from reservoir `<name>` into `:<var>` (or `<default>` if empty). |
| | `\advance_watermark <wm> [CHANNEL <ch>]` | Monotonically advance causal watermark on channel `0..63`. |
| | `\begin_dependent_write <wm> [CHANNEL <ch>]` | Register in-flight write at `<wm>`. |
| | `\end_dependent_write <wm> [CHANNEL <ch>]` | Complete in-flight write at `<wm>`. |
| | `\wait_until <ts> [dep=<dep> \| WATERMARK <dep> [CHANNEL <ch>]]` | Non-blockingly wait until replay clock reaches `<ts>` and/or watermark reaches `<dep>`. |
| **Subroutines, Sessions, Datasets, Quality & Pipelining (10)** | `\call <script_name> [<var>=<expr>...]` | Call another loaded `-f` script as a subroutine with scoped variables. |
| | `\session_connect [db=<db>] [user=<u>]` | Open or switch to a dynamic client database session. |
| | `\session_disconnect [<id>]` | Close dynamic client database session. |
| | `\dataset_bind <name> <mode> <v1>...` | Bind columns from `mmap` dataset (`wrap`, `once`, `partitioned`, `sample`, `AT <idx>`). |
| | `\copy_dataset <name> <table> [(cols)]` | Stream `mmap` dataset into `<table>` via `COPY FROM STDIN`. |
| | `\validate_result <actual> <expected> [[tol=]<t>]` | Assert `<actual> == <expected>` (within optional `<tol>`); updates `validation_passed`/`validation_failed`. |
| | `\record_recall [[name=]<m>] <expr>`<br>`\record_recall [[name=]<m>] <ret_var> <gt_var> [<k>]` | Record scalar recall expression or compute top-`K` set-intersection recall between `PGBT_ARRAY` variables and record in HDR/aging rings. |
| | `\startpipeline` / `\syncpipeline` / `\endpipeline` | Extended query protocol pipelining (`3` commands). |

---

## 4. Client-Side Expression Functions (61 Functions in `exprparse.y`)

- **Arithmetic, Type Coercion & Debugging (13)**: `abs(x)`, `least(x1, ...)`, `greatest(x1, ...)`, `int(x)`, `double(x)`, `sqrt(x)`, `ln(x)`, `exp(x)`, `pow(x, y)`, `power(x, y)`, `mod(x, y)`, `pi()`, `debug(x)`.
- **Random Distributions, Hashing & Permutations (8)**: `random(min, max)`, `random_gaussian(min, max, param)`, `random_exponential(min, max, param)`, `random_zipfian(min, max, s)`, `hash(val [, seed])`, `hash_murmur2(val [, seed])`, `hash_fnv1a(val [, seed])`, `permute(val, size [, seed])`.
- **Order-Entry, Strings, Bytea & Dates (9)**: `nurand(a, min, max [, c])`, `c_last(val)`, `random_astring(min_len, max_len)`, `random_nstring(min_len, max_len)`, `random_bytea(min_len, max_len)`, `format_string(fmt, ...)`, `to_timestamp(str)`, `date_add(base_date, offset, unit)`, `int_to_date(base_date, val, max_val [, interval_str])`.
- **Arrays, Vectors & Graphs (15)**: `array(e1, ...)`, `array_generate(len, min [, max])`, `vector(f1, ...)`, `vector_dims(v)`, `vector_norm(v)`, `vector_distance(v1, v2 [, metric])`, `random_vector(dim [, min, max, do_norm])`, `random_vector_gaussian(dim [, sigma, do_norm])`, `random_unit_vector(dim [, sigma, do_norm])`, `clustered_vector(dim, num_clusters [, sigma, seed, cluster_id])`, `planted_vector(dim, target_id [, delta, seed])`, `random_sparse_vector(dim, nnz [, min, max])`, `rmat_edge(scale_log2 [, a, b, c, seed])`, `ldbc_correlated_edge(src_id, num_vertices, num_communities, homophily_prob [, seed])`, `ldbc_flashmob_ts(base_ts_us, span_us [, num_spikes, spike_prob, seed])`.
- **Datasets, Watermarks, Reservoirs & ANN Quality (16)**: `dataset_count(name)` (`vec_dataset_count`), `dataset_get(name, row [, col])` (`vec_load`), `dataset_next_id(name [, mode])` (`vec_next_id`), `get_watermark([chan])`, `set_watermark(wm [, chan])`, `advance_watermark(wm [, chan])`, `begin_dependent_write(wm [, chan])`, `end_dependent_write(wm [, chan])`, `reservoir_push(name, val)`, `reservoir_sample(name [, default])`, `reservoir_count(name)`, `recall_at_k(retrieved_arr, truth_arr [, k])`, `ndcg_at_k(retrieved_arr, truth_arr [, k])`.

---

## 5. `contrib/pgbench` Server-Side SQL Functions (51 Functions in `SCHEMA pgbench`)

Running `CREATE EXTENSION IF NOT EXISTS pgbench;` installs `SCHEMA pgbench` with 51 C-backed SQL functions matching the client generators:

| Category | SQL Function Signatures in `SCHEMA pgbench` |
| :--- | :--- |
| **PRNG Seeding (3)** | `pgbench.setseed(float8)`, `pgbench.setseed(bigint)`, `pgbench.setseed(int4)` |
| **Random Distributions (5)** | `pgbench.random(min bigint, max bigint)`, `pgbench.random_gaussian(min bigint, max bigint, param float8)`, `pgbench.random_exponential(min bigint, max bigint, param float8)`, `pgbench.random_zipfian(min bigint, max bigint, param float8)`, `pgbench.random_poisson(center float8)` |
| **Hashes & Permutations (4)** | `pgbench.hash_murmur2(val bigint, seed bigint DEFAULT 0)`, `pgbench.hash_fnv1a(val bigint, seed bigint DEFAULT 0)`, `pgbench.hash(val bigint, seed bigint DEFAULT 0)`, `pgbench.permute(val bigint, size bigint, seed bigint DEFAULT 0)` |
| **Order-Entry, Strings & Bytea (7)** | `pgbench.nurand(a bigint, min bigint, max bigint [, c bigint])`, `pgbench.c_last(val bigint)`, `pgbench.random_astring(min_len bigint, max_len bigint)`, `pgbench.random_nstring(min_len bigint, max_len bigint)`, `pgbench.random_bytea(min_len bigint, max_len bigint)` *(alias `pgbench.pgbench_random_bytea`)* |
| **Dates, Formatting & Arrays (8)** | `pgbench.date_add(base_date text, offset_val bigint, unit text)`, `pgbench.int_to_date(base_date text, val bigint, unit text)`, `pgbench.int_to_date(base_date text, val bigint, max_val bigint, interval_str text)`, `pgbench.to_timestamp(str text)`, `pgbench.format_string(fmt text [, VARIADIC args "any"])`, `pgbench.array_generate(len bigint, val bigint [, max_val bigint])` |
| **Dense & Sparse Vectors (14)** | `pgbench.random_vector(dim int, min_val float8 DEFAULT -1.0, max_val float8 DEFAULT 1.0, do_normalize bool DEFAULT true)`, `pgbench.random_vector(dim int, dist text, do_normalize bool DEFAULT true)`, `pgbench.random_vector_gaussian(dim int, sigma float8 DEFAULT 1.0, do_normalize bool DEFAULT true)`, `pgbench.random_unit_vector(dim int)`, `pgbench.clustered_vector(dim int, num_clusters int, sigma float8 DEFAULT 0.1, seed bigint DEFAULT 0, cluster_id int DEFAULT -1)`, `pgbench.planted_vector(dim int, target_id bigint, delta float8 DEFAULT 0.01, seed bigint DEFAULT 0)`, `pgbench.random_sparse_vector(dim int, nnz int, min_val float8 DEFAULT 0.1, max_val float8 DEFAULT 1.0)` *(plus `pgbench.pgbench_*` aliases)* |
| **Graph Edges, Distance & Ground Truth (10)** | `pgbench.rmat_edge(scale_log2 int, a float8 DEFAULT 0.57, b float8 DEFAULT 0.19, c float8 DEFAULT 0.19, seed bigint DEFAULT 0)`, `pgbench.ldbc_correlated_edge(src_id bigint, num_vertices bigint, num_communities bigint DEFAULT 32, homophily_prob float8 DEFAULT 0.70, seed bigint DEFAULT 0)`, `pgbench.ldbc_flashmob_ts(base_ts_us bigint, span_us bigint, num_spikes bigint DEFAULT 16, spike_prob float8 DEFAULT 0.35, seed bigint DEFAULT 0)`, `pgbench.vector_distance(v1 text, v2 text, metric text DEFAULT 'l2')`, `pgbench.compute_ground_truth(items_table text, query_vector text, k int, metric text DEFAULT 'l2')` *(plus `pgbench.pgbench_*` aliases)* |

---

## 6. `--log-db` & Perffarm SQL Set-Returning Functions (`metrics_db_schema.sql`)

| Function Signature | Return Columns | Description |
| :--- | :--- | :--- |
| `pgbench_show_run()`<br>`pgbench_show_run(p_run_id bigint)`<br>`pgbench_show_run(p_run_label text)` | `(section text, scope text, metric text, value text, unit text, details text)` | 7-section vertical scorecard (`1. Run Info`, `2. Overall Metrics`, `3. Client Groups`, `4. Scripts`, `5. Recall & Aging`, `6. Commands`, `7. SLA Verifications`). |
| `pgbench_compare_runs(p_base_run_id bigint, p_cand_run_id bigint)`<br>`pgbench_compare_runs(p_base_label text, p_cand_label text)` | `(scope text, metric text, unit text, base_val double precision, cand_val double precision, abs_delta double precision, pct_change double precision, verdict text)` | Side-by-side comparison of two runs across overall, group, script, and recall metrics with automatic `'IMPROVED'` / `'REGRESSED'` / `'UNCHANGED'` verdicts. |
| `pgbench_compare_runs(p_run_ids bigint[])` | `(scope text, metric text, unit text, run_id bigint, run_label text, val double precision, delta_vs_base double precision, pct_vs_base double precision, sla_all_passed boolean)` | N-way run comparison matrix against `p_run_ids[1]` as baseline. |
| `pgbench_viewer_payload_json(p_mode text, p_run_id bigint, p_base_run_id bigint, p_cand_run_id bigint, p_run_ids bigint[], p_limit int)` | `jsonb` | Single-query JSONB payload generator used by `src/bin/pgbench/viewer/pgbench_viewer.py` (`p_mode`: `'overview'`, `'live'`, etc.). |
| `perffarm_compare_commits(p_sha_a text, p_sha_b text, p_animal text DEFAULT NULL, p_suite_filter text DEFAULT NULL)` | `(animal_name text, test_name text, suite_name text, metric text, unit text, sha_a_val double precision, sha_b_val double precision, abs_delta double precision, pct_change double precision, verdict text)` | Compares all matching tests and sequence steps (`test_name [rK Ds]`) between two git commit SHAs (supports prefix match) using the latest completed batch per animal & test. |
