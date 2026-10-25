# `pgbench` Benchmark Scripting Language, Client Expressions & `contrib/pgbench` Guide

This guide provides a complete reference for the `pgbench` benchmark scripting language (`src/bin/pgbench/script.c`, `commands.c`, `exprparse.y`, `variable.c`) and the matching server-side `contrib/pgbench` SQL function library (`SCHEMA pgbench`).

---

## 1. Runtime Type System (`PgBenchValue`) & Variable Interpolation

Every client variable (`:var`) holds a typed runtime value (`PgBenchValue`) alongside a cached SQL string representation:

| Type Tag | Description | Literal / Constructor Examples | SQL Interpolation Behavior (`:var`) |
| :--- | :--- | :--- | :--- |
| **`PGBT_INT`** | 64-bit signed integer (`int64`) | `42`, `-100`, `random(1, 10)` | Unquoted integer literal (`42`) |
| **`PGBT_DOUBLE`** | IEEE 754 double (`double`) | `3.14159`, `sqrt(2.0)` | Unquoted float literal (`3.14159`) |
| **`PGBT_BOOLEAN`** | Boolean (`true` / `false`) | `true`, `false`, `:a > 10` | `true` or `false` |
| **`PGBT_TEXT`** | Heap-allocated UTF-8 string | `'hello'`, `c_last(123)`, `random_astring(5, 10)` | Automatically SQL-escaped & single-quoted (`'BARBARPRI'`) when evaluated from an expression; raw CLI `-D` strings interpolate as-is for identifier/table substitution |
| **`PGBT_TIMESTAMP`** | Microseconds since `2000-01-01 00:00:00 UTC` | `to_timestamp('2025-01-01T00:00:00Z')` | ISO-8601 timestamp string |
| **`PGBT_ARRAY`** | 1D dynamic array of `PgBenchValue` | `array(1, 2, 3)`, `array_generate(5, 1, 100)`, `\gset_col` | PostgreSQL array literal (`'{1,2,3}'`); supports 0-based or 1-based subscripting (`:arr[:i]`) |
| **`PGBT_VECTOR`** | Dense float vector (`<= 4096` dims) | `vector(0.1, 0.2, 0.3)`, `random_vector(128)` | Bracketed vector literal (`'[0.1,0.2,0.3]'`) compatible with `pgvector` `::vector` |
| **`PGBT_BYTEA`** | Binary byte array | `random_bytea(16, 32)` | Hex-escaped bytea literal (`'\\x4a7f...'`) |
| **`PGBT_NULL`** | SQL / expression `NULL` | `null` | `NULL` |

### Built-In Client Variables
Every client automatically initializes the following variables:
- `:scale`: Current scale factor (`-s`).
- `:client_id`: 0-based client index (`0 .. nclients - 1`).
- `:random_seed`: Deterministic PRNG seed (`--random-seed`).
- `:default_seed`: Initial default seed.
- `:group_name` and `:stream_id`: Active `--group` name and 0-based stream index within the group.
- `:sqlstate` and `:error_message`: Populated whenever an error is caught by `\try ... \catch` or `\expect_error`.
- `:last_recall` and `:last_recall_<metric>`: Populated by `\record_recall`.
- `:last_validation_ok`: Boolean (`true` / `false`) populated by `\validate_result`.

---

## 2. Complete Backslash Meta-Command Reference (39 Meta-Commands)

### 2.1 Variable Assignment, Result Binding & Metadata Inspection

| Meta-Command | Syntax | Description |
| :--- | :--- | :--- |
| **`\set`** | `\set <var> <expr>` | Evaluates `<expr>` using the client expression engine and assigns the typed `PgBenchValue` result to `:<var>`. |
| **`\setshell`** | `\setshell <var> <cmd> [<arg> ...]` | Executes `<cmd>` in a subshell and assigns its integer stdout output to `:<var>`. |
| **`\shell`** | `\shell <cmd> [<arg> ...]` | Executes `<cmd>` in a subshell without capturing output. |
| **`\gset`** | `<SQL> \gset [<prefix>]` | Executes `<SQL>`, expects 1 row, and binds each column `<col>` to `:<prefix><col>`. Automatically parses `{...}` PostgreSQL arrays into `PGBT_ARRAY` (setting `:<prefix><col>_count`), `[...]` vectors into `PGBT_VECTOR`, and `\x...` into `PGBT_BYTEA`. |
| **`\aset`** | `<SQL1> \; <SQL2> \aset [<prefix>]` | When `<prefix>` is omitted, binds the last row of each query in a `\;` compound statement. When `<prefix>` is supplied, binds **all rows** of each column `<col>` into a `PGBT_ARRAY` variable `:<prefix><col>`. |
| **`\gset_col`** | `<SQL> \gset_col <var> [<col_name_or_idx>]` | Executes `<SQL>` and captures **all rows** of a single column (default column `0`) into a 1D `PGBT_ARRAY` variable `:<var>` (up to `100,000` rows). Ideal for capturing ranked ID lists for `\record_recall`. |
| **`\gset_meta`** | `<SQL> \gset_meta [<prefix>]` | Attaches metadata capture to the preceding SQL command (default `<prefix>` is `meta`), populating `:<prefix>_status` (`"OK"` or `"ERROR"`), `:<prefix>_sqlstate`, and `:<prefix>_nrows` (plus `:<prefix>_<col>` from the last returned row when the query returns tuples and does not already use `\gset` / `\aset`). |

### 2.2 Control Flow & Loops (`\if`, `\for`, `\while`)

```sql
-- Conditional branching (note: equality uses SQL '=', inequality uses '<>' or '!=')
\if :scale > 1 AND random(1, 100) <= 15
  \set supply_w_id random(1, :scale)
\elif :scale = 1
  \set supply_w_id 1
\else
  \set supply_w_id :w_id
\endif

-- Bounded integer loop (\for <var> <start_expr> .. <end_expr> [BY <step_expr>])
\for d_id 1 .. 10 BY 1
  CALL pgbench_c_delivery(:w_id, :d_id, :carrier_id);
\endfor

-- Condition-driven loop (\while <expr> ... \endwhile, up to MAX_LOOP_DEPTH)
\set attempt 0
\while :attempt < 3
  \set attempt :attempt + 1
\endwhile
```

### 2.3 Structured Exception Handling (`\try`, `\catch`, `\endtry`, `\expect_error`)

In complex OLTP and financial graph workloads (such as the 1% invalid-item rollback in `pgbench-c`, limit-order rollbacks in `pgbench-e`, and conditional fraud-detection rollbacks in `FinBench TRW`), certain transactions are expected to roll back by design.

```sql
\try
  CALL pgbench_c_new_order(:w_id, :d_id, :c_id, :ol_cnt, :invalid_flag);
\catch 'P0001'
  -- Executed only if SQLSTATE matches 'P0001' (omit 'SQLSTATE' to catch any SQL error)
  -- :sqlstate and :error_message are automatically populated
  -- Increments expected_rollbacks instead of transactions_failed!
  ROLLBACK;
\endtry

-- Single-statement shorthand for expected errors:
\expect_error '23505'
INSERT INTO unique_test VALUES (1);
```

### 2.4 Pacing, Keying Time & Think Time (`\sleep`, `\keying_time`, `\think_time`)

All three sleep commands are executed non-blockingly by `poller.c` and accept arbitrary arithmetic/function expressions plus an optional unit (`us`, `ms`, `s`; default `s` for `\keying_time`/`\think_time`, `us` or `s` for `\sleep`):

- **`\sleep <expr> [us|ms|s]`**: Pauses the client for `<expr>` duration (included in transaction cycle time).
- **`\keying_time <expr> [us|ms|s]`**: Simulates operator terminal keying delay before a transaction; tracked in `keying time average` and **subtracted** when computing database `response time average`.
- **`\think_time <expr> [us|ms|s]`**: Simulates user think time after a transaction; tracked in `think time average` and **subtracted** when computing database `response time average`.

### 2.5 Cross-Client Coordination: Barriers, Reservoirs & Causal Watermarks

| Meta-Command | Syntax | Description |
| :--- | :--- | :--- |
| **`\phase_barrier`** | `\phase_barrier [<phase_name>]` | Blocks the client until **all active clients across all worker threads** reach the barrier. Used in `pgbench_ldbc_*_snb_bi.sql` to separate `Power`, `Batch Update`, and `Throughput` phases. |
| **`\reservoir_push`** | `\reservoir_push <name> <expr>` | Atomically pushes an integer ID (or `PGBT_ARRAY` of IDs) into the shared lock-free reservoir `<name>` (configured via `--reservoir=<name>:<capacity>`). |
| **`\reservoir_sample`** | `\reservoir_sample <name> <var> [<default_expr>]` | Uniformly samples a live ID from reservoir `<name>` into `:<var>` (or assigns `<default_expr>` if the reservoir is currently empty). |
| **`\advance_watermark`** | `\advance_watermark <wm_expr> [CHANNEL <chan_expr>]` | Monotonically advances the global (or channel `0..63`) causal watermark to `<wm_expr>`. |
| **`\begin_dependent_write`** | `\begin_dependent_write <wm_expr> [CHANNEL <chan_expr>]` | Registers an in-flight write at timestamp `<wm_expr>` so the contiguous minimum watermark does not advance past uncommitted writes. |
| **`\end_dependent_write`** | `\end_dependent_write <wm_expr> [CHANNEL <chan_expr>]` | Completes an in-flight write at `<wm_expr>`, advancing the contiguous minimum watermark once all earlier writes have finished. |
| **`\wait_until`** | `\wait_until <ts_expr> [WATERMARK <dep_expr> [CHANNEL <chan_expr>]]` | Non-blockingly pauses the client until the replay clock reaches `<ts_expr>` and/or the causal watermark on `<chan_expr>` reaches `<dep_expr>`. |

### 2.6 Subroutines, Dynamic Sessions, Datasets, Pipelining & Quality Telemetry

| Meta-Command | Syntax | Description |
| :--- | :--- | :--- |
| **`\call`** | `\call <script_name> [<var>=<expr> ...]` | Invokes another loaded script (`-f <script>@<weight>`) as a subroutine with a pushed `VariableSpace` scope and returns to the caller upon completion. |
| **`\session_connect`** | `\session_connect [db=<dbname>] [user=<user>]` | Opens or switches to a dynamic client connection (bounded globally by `--max-concurrent-sessions=<N>`). |
| **`\session_disconnect`** | `\session_disconnect [<session_id>]` | Closes the current or specified dynamic session connection. |
| **`\dataset_bind`** | `\dataset_bind <name> <wrap\|once\|partitioned\|sample\|AT <expr>> <var1> [<var2> ...]` | Binds columns from a memory-mapped `--dataset` or `--vector-dataset` into `:<var1>`, `:<var2>`, ... (`wrap`/`next`, `once`, `partitioned`, `sample`/`random`, or `AT <row_idx>`). |
| **`\copy_dataset`** | `\copy_dataset <name> <table> [(col1,...)]` | Streams memory-mapped dataset `<name>` into `<table>` via `COPY ... FROM STDIN`. |
| **`\validate_result`** | `\validate_result <actual_expr> <expected_expr> [tol=<tol_expr>]` | Asserts that `<actual_expr>` equals `<expected_expr>` (within optional floating-point `<tol_expr>`), sets `:last_validation_ok`, and increments `validation_passed` or `validation_failed`. |
| **`\record_recall`** | `\record_recall [[name=]<metric>] <retrieved_var> <truth_var> [<k_expr>]` | Computes top-`K` set-intersection recall between `PGBT_ARRAY` variables `:<retrieved_var>` and `:<truth_var>`, sets `:last_recall`, and updates global + named recall histograms and Q1/Q4 aging ring buffers. |
| **Pipelining** | `\startpipeline` / `\syncpipeline` / `\endpipeline` | Starts, flushes, or ends a libpq extended-query pipeline block. |

---

## 3. All 61 Client-Side Expression Functions (`src/bin/pgbench/exprparse.y`)

These functions execute directly inside the `pgbench` client process (in `\set`, `\if`, `\while`, `\for`, `\sleep`, `\validate_result`, and `\record_recall`) with zero database round-trips:

### 3.1 Arithmetic, Type Coercion & Debugging (13)
- `abs(x)`: Absolute value of `x`.
- `least(x1, x2, ...)` / `greatest(x1, x2, ...)`: Minimum or maximum across variadic arguments.
- `int(x)` / `double(x)`: Casts `x` to 64-bit integer (`PGBT_INT`) or double (`PGBT_DOUBLE`).
- `sqrt(x)`, `ln(x)`, `exp(x)`, `pow(x, y)` / `power(x, y)`, `mod(x, y)`: Standard mathematical operations.
- `pi()`: Returns π (`3.14159265358979323846`).
- `debug(x)`: Prints the value of `x` to `stderr` and returns `x`.

### 3.2 Random Distributions, Hashing & Permutations (8)
- `random(min, max)`: Uniform random integer in `[min, max]`.
- `random_gaussian(min, max, param)`: Gaussian (normal) random integer in `[min, max]` (`param >= 2.0`).
- `random_exponential(min, max, param)`: Exponential random integer in `[min, max]` (`param > 0.0`).
- `random_zipfian(min, max, s)`: Zipfian power-law random integer in `[min, max]` (`s != 1.0`).
- `hash(val [, seed])` / `hash_murmur2(val [, seed])`: 64-bit MurmurHash2.
- `hash_fnv1a(val [, seed])`: 64-bit FNV-1a hash.
- `permute(val, size [, seed])`: Bijective Feistel-like pseudorandom permutation mapping `[0, size)` onto `[0, size)`.

### 3.3 Order-Entry, String, Bytea & Calendar Generators (9)
- `nurand(a, min, max [, c])`: Non-uniform random integer `(((random(0, a) | random(min, max)) + c) % (max - min + 1)) + min`.
- `c_last(val)`: Generates 3-syllable customer last name (`'BARBARBAR'` .. `'EINGEINGEING'`) from `0..999`.
- `random_astring(min_len, max_len)`: Random alphanumeric string (`a-z`, `A-Z`, `0-9`).
- `random_nstring(min_len, max_len)`: Random numeric digit string (`0-9`).
- `random_bytea(min_len, max_len)`: Random binary `PGBT_BYTEA` value.
- `format_string(fmt, ...)`: `printf`-style string formatting (`%d`, `%lld`, `%f`, `%s`).
- `to_timestamp(str)`: Parses ISO-8601 string or epoch value into `PGBT_TIMESTAMP`.
- `date_add(base_date, offset, unit)`: Adds `offset` (`'seconds'`, `'minutes'`, `'hours'`, `'days'`, `'months'`, `'years'`) to `base_date`.
- `int_to_date(base_date, val, max_val [, interval_str])`: Linearly maps `val` in `[0, max_val]` across a date window.

### 3.4 Array, Vector & Graph Generators (15)
- `array(e1, e2, ...)`: Constructs a 1D `PGBT_ARRAY`.
- `array_generate(len, min [, max])`: Generates a 1D `PGBT_ARRAY` of length `len` filled with constant `min` or random integers in `[min, max]`.
- `vector(f1, f2, ...)`: Constructs a dense `PGBT_VECTOR`.
- `vector_dims(v)` / `vector_norm(v)`: Returns dimension count or Euclidean L2 norm of vector `v`.
- `vector_distance(v1, v2 [, metric])`: Computes `'l2'`, `'cosine'`, or `'ip'` (inner product) distance between two vectors on the client.
- `random_vector(dim [, min, max, do_norm])`: Uniform random vector of dimension `dim`.
- `random_vector_gaussian(dim [, sigma, do_norm])`: Gaussian random vector.
- `random_unit_vector(dim [, sigma, do_norm])`: Unit-normalized random vector on the `(dim - 1)`-sphere.
- `clustered_vector(dim, num_clusters [, sigma, seed, cluster_id])`: Gaussian cluster vector around deterministic centroid `cluster_id`.
- `planted_vector(dim, target_id [, delta, seed])`: Query vector planted at perturbation `delta` from `target_id`.
- `random_sparse_vector(dim, nnz [, min, max])`: Sparse vector literal (`'{i:v,...}/dim'`).
- `rmat_edge(scale_log2 [, a, b, c, seed])`: Generates a 2-element `PGBT_ARRAY` `{src, dst}` following the R-MAT power-law graph model.
- `ldbc_correlated_edge(src_id, num_vertices, num_communities, homophily_prob [, seed])`: Community-correlated target vertex ID.
- `ldbc_flashmob_ts(base_ts_us, span_us [, num_spikes, spike_prob, seed])`: Timestamp with flashmob burst spikes.

### 3.5 Datasets, Reservoirs, Watermarks & ANN Quality (16)
- **Datasets (6)**: `dataset_count(name)` (alias `vec_dataset_count`), `dataset_get(name, row [, col])` (alias `vec_load`), `dataset_next_id(name [, mode])` (alias `vec_next_id`).
- **Watermarks (5)**: `get_watermark([chan])`, `set_watermark(wm [, chan])`, `advance_watermark(wm [, chan])`, `begin_dependent_write(wm [, chan])`, `end_dependent_write(wm [, chan])`.
- **Reservoirs (3)**: `reservoir_push(name, val)`, `reservoir_sample(name [, default])`, `reservoir_count(name)`.
- **ANN Quality (2)**: `recall_at_k(retrieved_arr, truth_arr [, k])`, `ndcg_at_k(retrieved_arr, truth_arr [, k])`.

---

## 4. All 51 Server-Side `contrib/pgbench` SQL Functions (`SCHEMA pgbench`)

Running `CREATE EXTENSION IF NOT EXISTS pgbench;` installs the `pgbench` schema containing 51 SQL function overloads backed by `src/common/pgbench_funcs.c`. Because they share the exact same C implementation as the `pgbench` client, in-database `INSERT INTO ... SELECT ... FROM generate_series(...)` initialization produces identical distributions at native C speed:

| Category | SQL Function Signatures in `SCHEMA pgbench` |
| :--- | :--- |
| **PRNG Seeding (3)** | • `pgbench.setseed(double precision) RETURNS void`<br>• `pgbench.setseed(bigint) RETURNS void`<br>• `pgbench.setseed(integer) RETURNS void` |
| **Random Distributions (5)** | • `pgbench.random(min bigint, max bigint) RETURNS bigint`<br>• `pgbench.random_gaussian(min bigint, max bigint, parameter float8) RETURNS bigint`<br>• `pgbench.random_exponential(min bigint, max bigint, parameter float8) RETURNS bigint`<br>• `pgbench.random_zipfian(min bigint, max bigint, parameter float8) RETURNS bigint`<br>• `pgbench.random_poisson(center float8) RETURNS bigint` |
| **Hashes & Permutations (4)** | • `pgbench.hash_murmur2(val bigint, seed bigint DEFAULT 0) RETURNS bigint`<br>• `pgbench.hash_fnv1a(val bigint, seed bigint DEFAULT 0) RETURNS bigint`<br>• `pgbench.hash(val bigint, seed bigint DEFAULT 0) RETURNS bigint`<br>• `pgbench.permute(val bigint, size bigint, seed bigint DEFAULT 0) RETURNS bigint` |
| **Order-Entry, Strings & Bytea (7)** | • `pgbench.nurand(a bigint, min bigint, max bigint) RETURNS bigint`<br>• `pgbench.nurand(a bigint, min bigint, max bigint, c bigint) RETURNS bigint`<br>• `pgbench.c_last(val bigint) RETURNS text`<br>• `pgbench.random_astring(min_len bigint, max_len bigint) RETURNS text`<br>• `pgbench.random_nstring(min_len bigint, max_len bigint) RETURNS text`<br>• `pgbench.random_bytea(min_len bigint, max_len bigint) RETURNS bytea` *(alias `pgbench.pgbench_random_bytea`)* |
| **Dates, Formatting & Arrays (8)** | • `pgbench.date_add(base_date text, offset_val bigint, unit text) RETURNS text`<br>• `pgbench.int_to_date(base_date text, val bigint, unit text) RETURNS text`<br>• `pgbench.int_to_date(base_date text, val bigint, max_val bigint, interval_str text) RETURNS text`<br>• `pgbench.to_timestamp(str text) RETURNS text`<br>• `pgbench.format_string(fmt text) RETURNS text`<br>• `pgbench.format_string(fmt text, VARIADIC args "any") RETURNS text`<br>• `pgbench.array_generate(len bigint, val bigint) RETURNS bigint[]`<br>• `pgbench.array_generate(len bigint, min_val bigint, max_val bigint) RETURNS bigint[]` |
| **Dense & Sparse Vectors (14)** | • `pgbench.random_vector(dim int, min_val float8 DEFAULT -1.0, max_val float8 DEFAULT 1.0, do_normalize bool DEFAULT true) RETURNS text`<br>• `pgbench.random_vector(dim int, dist text, do_normalize bool DEFAULT true) RETURNS text`<br>• `pgbench.random_vector_gaussian(dim int, sigma float8 DEFAULT 1.0, do_normalize bool DEFAULT true) RETURNS text`<br>• `pgbench.random_unit_vector(dim int) RETURNS text`<br>• `pgbench.clustered_vector(dim int, num_clusters int, sigma float8 DEFAULT 0.1, seed bigint DEFAULT 0, cluster_id int DEFAULT -1) RETURNS text`<br>• `pgbench.planted_vector(dim int, target_id bigint, delta float8 DEFAULT 0.01, seed bigint DEFAULT 0) RETURNS text`<br>• `pgbench.random_sparse_vector(dim int, nnz int, min_val float8 DEFAULT 0.1, max_val float8 DEFAULT 1.0) RETURNS text`<br>*(Each function also provides a `pgbench.pgbench_*` alias for backward compatibility.)* |
| **Graph Edges, Distance & Ground Truth (10)** | • `pgbench.rmat_edge(scale_log2 int, a float8 DEFAULT 0.57, b float8 DEFAULT 0.19, c float8 DEFAULT 0.19, seed bigint DEFAULT 0) RETURNS bigint[]`<br>• `pgbench.ldbc_correlated_edge(src_id bigint, num_vertices bigint, num_communities bigint DEFAULT 32, homophily_prob float8 DEFAULT 0.70, seed bigint DEFAULT 0) RETURNS bigint`<br>• `pgbench.ldbc_flashmob_ts(base_ts_us bigint, span_us bigint, num_spikes bigint DEFAULT 16, spike_prob float8 DEFAULT 0.35, seed bigint DEFAULT 0) RETURNS bigint`<br>• `pgbench.vector_distance(v1 text, v2 text, metric text DEFAULT 'l2') RETURNS float8`<br>• `pgbench.compute_ground_truth(items_table text, query_vector text, k int, metric text DEFAULT 'l2') RETURNS integer[]`<br>*(Each function also provides a `pgbench.pgbench_*` alias.)* |
