# `log2_hist` — 64-Bucket RLE-Compressed `log2(V+1)` Histogram Data Type

`log2_hist` is a PostgreSQL base type extension implementing a 64-bucket run-length-encoded (RLE) $\log_2(V+1)$ histogram of 64-bit unsigned counters (`uint64[64]`), indexed over 0-based buckets `0..63`.

---

## 1. Core Properties

1. **Logical Model (Expanded Form)**:
   - An array of 64 unsigned 64-bit integer counters (`uint64[64]`), indexed over 0-based buckets `0..63`.
2. **Logarithmic Bucket Mapping (`64 - clz64(V)`)**:
   - A 64-bit observation $V \in [0, 2^{63}-1]$ maps to bucket index:
     - $V = 0 \rightarrow$ bucket `0`
     - $V > 0 \rightarrow 64 - \text{clz}_{64}(V) = \lfloor \log_2(V) \rfloor + 1 \in [1, 63]$
   - Negative inputs ($V < 0$) raise `ERRCODE_INVALID_PARAMETER_VALUE` by default, or clamp to bucket `0` when the 2nd argument `clamp_negative` is `true` (or when `SET log2_hist.clamp_negative = on` is set).
3. **Run-Length Compressed (`varlena`) Binary Storage**:
   - Skips empty (`0`) buckets using a compact array of `(skip_zeros, run_len)` descriptors followed by an 8-byte-aligned `uint64[]` counter payload (`'[]'` is 8 bytes in memory; 5 bytes on disk with a 1-byte short `varlena` header).
4. **Canonical Textual Representation**:
   - `'[(nr-of-zeros){count[,count]*}]'` — zero or more pairs of `(nr-of-zeros)` skipped empty buckets followed by comma-separated non-zero bucket values in `{...}` (e.g., `'[]'`, `'[(0){5,10}]'`, `'[(3){10}(1){20}]'`).
   - Non-canonical inputs (embedded zeros inside `{...}`, adjacent runs separated by `(0)`, trailing zeros, or extra whitespace) are automatically normalized on input.
5. **0-Based (`0..63`) Raw Subscripting & Live-Range `bigint[]` Casting**:
   - Direct subscripting `h[0..63]`, slicing `h[lo:hi]`, and subscript assignment `h[b] = cnt`.
   - `h::bigint[]` returns a 1-D `bigint[]` with a **0-based lower bound** spanning only the live `[b_first:b_last]` non-zero bucket range (or `'{}'` when empty).
   - `log2_hist_buckets(h)` / `log2_hist_to_array(h, true, 0)` returns the full 64-element `[0:63]` `bigint[]`.
   - `arr::log2_hist` compresses any 1-D `bigint[]` of length $\le 64$, respecting its 0-based `lbound` (and also accepting 64-element 1-based `[1:64]` SQL arrays).
6. **Saturating Arithmetic & Overflow Semantics**:
   - Histogram merging (`a + b` / `log2_hist_merge(a, b)`) and observation addition use branchless (`ADD` + `CMOVC`) or AVX-512 SIMD saturating 64-bit unsigned addition clamped at `UINT64_MAX` (`18446744073709551615`).
   - `bigint[]` conversions copy 64-bit words directly without per-element branching (values $> 2^{63}-1$ reinterpret in two's complement as signed `bigint`).

---

## 2. Bucket Range Reference (`0..63`)

| Bucket Index (`b`) | Leading Zeros (`clz64(V)`) | Value Range `[V_min, V_max]` | Bucket Width | Approximate Scale (when `V` is in ns) |
| :--- | :--- | :--- | :--- | :--- |
| **0** | 64 | `0` | `1` | `0 ns` |
| **1** | 63 | `1` | `1` ($2^0$) | `1 ns` |
| **2** | 62 | `2 .. 3` | `2` ($2^1$) | `2 .. 3 ns` |
| **3** | 61 | `4 .. 7` | `4` ($2^2$) | `4 .. 7 ns` |
| **4** | 60 | `8 .. 15` | `8` ($2^3$) | `8 .. 15 ns` |
| **5** | 59 | `16 .. 31` | `16` ($2^4$) | `16 .. 31 ns` |
| **10** | 54 | `512 .. 1,023` | `512` ($2^9$) | `~0.51 .. 1.02 us` |
| **20** | 44 | `524,288 .. 1,048,575` | $2^{19}$ | `~0.52 .. 1.05 ms` |
| **30** | 34 | $2^{29} \dots 2^{30}-1$ | $2^{29}$ | `~0.54 .. 1.07 s` |
| **40** | 24 | $2^{39} \dots 2^{40}-1$ | $2^{39}$ | `~9.16 .. 18.3 min` |
| **50** | 14 | $2^{49} \dots 2^{50}-1$ | $2^{49}$ | `~6.51 .. 13.0 days` |
| **63** | 1 | $2^{62} \dots 2^{63}-1$ | $2^{62}$ | `~146.2 .. 292.5 years` (`INT64_MAX`) |

---

## 3. Operations & Functions Reference

### 3.1 Operators & Subscripting

| Syntax | Return Type | C Function | Description |
| :--- | :--- | :--- | :--- |
| `h[b]` | `bigint` | `log2_hist_subscript_fetch` | 0-based (`0..63`) bucket counter lookup in $O(\text{runs})$ time (`0` for empty buckets, `NULL` if $b < 0$ or $b > 63$). |
| `h[lo:hi]` | `log2_hist` | `log2_hist_subscript_fetch_slice` | Slice extraction returning a `log2_hist` with only buckets `lo..hi` retained (supports open bounds `h[:hi]`, `h[lo:]`, `h[:]`). |
| `SET h[b] = cnt` | `log2_hist` | `log2_hist_subscript_assign` | Subscript assignment in `UPDATE` / PL/pgSQL setting bucket `b` (`0..63`) to `cnt` and re-normalizing RLE runs. |
| `a + b` | `log2_hist` | `log2_hist_merge` | Pointwise saturating `uint64` addition of two `log2_hist` histograms (clamped at `UINT64_MAX`). |
| `h + val`<br>`val + h` | `log2_hist` | `log2_hist_add_2`<br>`log2_hist_add_rev` | Adds a single `bigint` observation `val` into bucket $64 - \text{clz}_{64}(\text{val})$ (commutator supported). |
| `=`, `<>`, `<`, `<=`, `>=`, `>` | `boolean` | `log2_hist_eq`, `log2_hist_cmp` | Structural equality and lexicographical bucket-by-bucket (`0..63`) ordering; backed by default `btree` (`log2_hist_btree_ops`) and `hash` (`log2_hist_hash_ops`) operator classes for `DISTINCT`, `GROUP BY`, `ORDER BY`, and indexing. |

### 3.2 Casts & Array Conversions

| Cast / Function | Return Type | C Function | Description |
| :--- | :--- | :--- | :--- |
| `h::bigint[]`<br>`log2_hist_to_int8array(h)` | `bigint[]` | `log2_hist_to_int8array` | Expands `h` into a 1-D `bigint[]` with a **0-based lower bound** spanning only the live `[b_first:b_last]` non-zero bucket range (or `'{}'` when empty). |
| `arr::log2_hist`<br>`int8array_to_log2_hist(arr)` | `log2_hist` | `int8array_to_log2_hist` | Compresses any 1-D `bigint[]` (length $\le 64$) into `log2_hist`, respecting its 0-based `lbound` (and also accepting full 64-element 1-based `[1:64]` SQL arrays). |
| `log2_hist_buckets(h)` | `bigint[]` | `log2_hist_buckets` | Expands `h` into the full 64-element 0-based `bigint[]` (`[0:63]={...}`). |
| `log2_hist_to_array(h, full_range DEFAULT true, index_base DEFAULT 0)` | `bigint[]` | `log2_hist_to_array` | Configurable expansion supporting either full 64-bucket (`true`) or live-range (`false`) output and either 0-based (`0..63`) or 1-based (`1..64`) array bounds. |

### 3.3 Constructors, Bucket Mapping & Mutation Functions

| Function Signature | Return Type | C Function | Description |
| :--- | :--- | :--- | :--- |
| `log2_hist_bucket(val bigint)`<br>`log2_hist_bucket(val bigint, clamp_negative boolean)` | `integer` | `log2_hist_bucket_1`<br>`log2_hist_bucket_2` | Returns bucket index $64 - \text{clz}_{64}(\text{val}) \in [0, 63]$. Raises error on $\text{val} < 0$ unless `clamp_negative` is `true` (or GUC `log2_hist.clamp_negative = on`), which clamps to bucket `0`. |
| `log2_hist_empty()` | `log2_hist` | `log2_hist_empty` | Returns an empty histogram `'[]'`. |
| `log2_hist_single(val bigint)`<br>`log2_hist_single(val bigint, clamp_negative boolean, count bigint DEFAULT 1)` | `log2_hist` | `log2_hist_single_1`<br>`log2_hist_single_3` | Constructs a histogram containing a single observation `val` with frequency `count`. |
| `log2_hist_add(h log2_hist, val bigint)`<br>`log2_hist_add(h log2_hist, val bigint, clamp_negative boolean, count bigint DEFAULT 1)` | `log2_hist` | `log2_hist_add_2`<br>`log2_hist_add_4` | Adds `count` observations of `val` to `h` with saturating `uint64` arithmetic. |
| `log2_hist_merge(a log2_hist, b log2_hist)`<br>`log2_hist_add(a log2_hist, b log2_hist)` | `log2_hist` | `log2_hist_merge` | Merges two histograms via pointwise saturating `uint64` addition. |

### 3.4 Inspection, Analytics & Wire Functions

| Function Signature | Return Type | C Function | Description |
| :--- | :--- | :--- | :--- |
| `log2_hist_total_count(h log2_hist)` | `bigint` | `log2_hist_total_count` | Saturating sum of all non-zero bucket counters in `h`. |
| `log2_hist_min_bucket(h log2_hist)` | `integer` | `log2_hist_min_bucket` | Lowest non-zero bucket index (`0..63`), or `NULL` if empty ($O(1)$ from `runs[0].skip_zeros`). |
| `log2_hist_max_bucket(h log2_hist)` | `integer` | `log2_hist_max_bucket` | Highest non-zero bucket index (`0..63`), or `NULL` if empty ($O(\text{runs})$). |
| `log2_hist_percentile(h log2_hist, p float8)` | `bigint` | `log2_hist_percentile` | Estimates quantile $p \in [0.0, 1.0]$ using linear interpolation within the target bucket $[2^{b-1}, 2^b - 1]$ (`NULL` if empty). |
| `log2_hist_in(cstring)` / `log2_hist_out(log2_hist)` | `log2_hist` / `cstring` | `log2_hist_in` / `log2_hist_out` | Text parser and canonical serializer for `'[(nr-of-zeros){count[,count]*}]'`. |
| `log2_hist_recv(internal)` / `log2_hist_send(log2_hist)` / `log2_hist_from_bytea(bytea)` | `log2_hist` / `bytea` | `log2_hist_recv` / `log2_hist_send` / `log2_hist_from_bytea` | Binary wire-format serialization and `bytea` deserialization helper. |

### 3.5 Parallel-Safe Aggregates & GUC Settings

| Aggregate / GUC | Type | Description |
| :--- | :--- | :--- |
| `log2_hist_agg(val bigint)`<br>`log2_hist_agg(val bigint, clamp_negative boolean)` | `log2_hist` | Parallel-safe aggregate accumulating raw `bigint` observations into an in-memory `uint64[64]` state (512 bytes) and RLE-compressing once in `FINALFUNC`. |
| `log2_hist_merge_agg(h log2_hist)` | `log2_hist` | Parallel-safe aggregate merging multiple `log2_hist` rows into a single combined `log2_hist`. |
| `log2_hist.clamp_negative` | `boolean` GUC | Session/user setting (`off` by default) controlling whether negative inputs to single-argument functions/operators clamp to bucket `0` (`on`) or raise an error (`off`). |

---

## 4. Usage Examples

```sql
CREATE EXTENSION log2_hist;

-- 1. Build a histogram by adding observations (0->b0, 1->b1, 2..3->b2, 4..7->b3, 8..15->b4)
SELECT log2_hist_empty() + 0 + 1 + 2 + 3 + 4 + 7 + 8 AS h;
-- Result: [(0){1,1,2,2,1}]

-- 2. Direct 0-based (0..63) subscripting and slicing on raw log2_hist values
SELECT
    ('[(3){10,20}(1){30}]'::log2_hist)[3] AS b3,
    ('[(3){10,20}(1){30}]'::log2_hist)[4] AS b4,
    ('[(3){10,20}(1){30}]'::log2_hist)[5] AS b5_empty,
    ('[(3){10,20}(1){30}]'::log2_hist)[6] AS b6,
    ('[(3){10,20}(1){30}]'::log2_hist)[3:4] AS slice_3_4;
-- Result: b3 = 10, b4 = 20, b5_empty = 0, b6 = 30, slice_3_4 = [(3){10,20}]

-- 3. Cast to 0-based live-range bigint[] vs. full 64-element expansion
SELECT
    ('[(3){10}(1){20}]'::log2_hist)::bigint[] AS live_range_arr,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])::log2_hist AS roundtrip_hist,
    cardinality(log2_hist_buckets('[(3){10}(1){20}]'::log2_hist)) AS full_64_len;
-- Result: live_range_arr = [3:5]={10,0,20}, roundtrip_hist = [(3){10}(1){20}], full_64_len = 64

-- 4. Aggregate observations and compute percentiles
SELECT
    log2_hist_agg(latency_ns) AS hist,
    log2_hist_total_count(log2_hist_agg(latency_ns)) AS total_samples,
    log2_hist_min_bucket(log2_hist_agg(latency_ns)) AS min_bucket,
    log2_hist_max_bucket(log2_hist_agg(latency_ns)) AS max_bucket,
    log2_hist_percentile(log2_hist_agg(latency_ns), 0.50) AS p50_ns,
    log2_hist_percentile(log2_hist_agg(latency_ns), 0.99) AS p99_ns
FROM (VALUES (120), (250), (510), (1050), (4200), (15000)) AS t(latency_ns);
```

---

## 5. Building and Testing

```bash
make -C contrib/log2_hist PG_CONFIG=/path/to/pg_config all install
make -C contrib/log2_hist PG_CONFIG=/path/to/pg_config installcheck
```
