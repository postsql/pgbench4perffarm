CREATE EXTENSION log2_hist;

-- =========================================================================
-- 1. Bucket Index Formula: bucket(V) = 64 - clz64(V) over 0..63
-- =========================================================================
SELECT
    log2_hist_bucket(0) AS b0,
    log2_hist_bucket(1) AS b1,
    log2_hist_bucket(2) AS b2_lo,
    log2_hist_bucket(3) AS b2_hi,
    log2_hist_bucket(4) AS b3_lo,
    log2_hist_bucket(7) AS b3_hi,
    log2_hist_bucket(8) AS b4_lo,
    log2_hist_bucket(15) AS b4_hi,
    log2_hist_bucket(9223372036854775807) AS b63_max;

-- Verify boundaries for all 64 buckets (k = 1..62: 2^(k-1)..2^k-1 -> bucket k)
SELECT bool_and(
    log2_hist_bucket((1::bigint << (k - 1))) = k AND
    log2_hist_bucket(((1::bigint << k) - 1)) = k
) AS all_boundaries_ok
FROM generate_series(1, 62) AS k;

SELECT
    log2_hist_bucket(1::bigint << 62) AS b63_lo,
    log2_hist_bucket(9223372036854775807) AS b63_hi;

-- Negative input handling: 2nd argument and GUC flag
SELECT log2_hist_bucket(-1, true) AS neg_clamped,
       log2_hist_bucket(-9223372036854775808, true) AS int64_min_clamped;

SET log2_hist.clamp_negative = on;
SELECT log2_hist_bucket(-1) AS guc_neg_clamped,
       ('[]'::log2_hist + (-500)) AS guc_add_neg;
RESET log2_hist.clamp_negative;

-- =========================================================================
-- 2. Textual I/O, Binary Send/Recv & RLE Canonicalization
-- =========================================================================
SELECT
    '[]'::log2_hist AS empty_hist,
    pg_column_size('[]'::log2_hist) AS empty_mem_bytes,
    '[(0){5,10,15}]'::log2_hist AS run_at_0,
    '[(3){10}(1){20}(4){30,40}]'::log2_hist AS multi_run,
    '[(63){99}]'::log2_hist AS bucket_63_only;

-- Non-canonical inputs must normalize to canonical RLE output
SELECT
    '[(2){10,0,20}]'::log2_hist AS split_zero_inside_run,
    '[(2){10}(0){20}]'::log2_hist AS merged_adjacent_runs,
    '[(2){10,0,0}]'::log2_hist AS stripped_trailing_zeros,
    '[(5){0,0}(3){0}]'::log2_hist AS all_zero_collapsed,
    ' [ ( 1 ) { 10 , 20 } ( 2 ) { 30 } ] '::log2_hist AS whitespace_normalized,
    '[(0){18446744073709551615}]'::log2_hist AS uint64_max_bucket;

-- Binary wire format verification via log2_hist_send() / log2_hist_from_bytea()
SELECT
    encode(log2_hist_send('[]'::log2_hist), 'hex') AS empty_wire_hex,
    encode(log2_hist_send('[(3){10}(1){20}]'::log2_hist), 'hex') AS run2_wire_hex;

SELECT
    bool_and(h = log2_hist_from_bytea(log2_hist_send(h))) AS binary_roundtrip_all_ok
FROM (
    VALUES
        ('[]'::log2_hist),
        ('[(0){1,2,3}]'::log2_hist),
        ('[(3){10}(1){20}(50){18446744073709551615}]'::log2_hist),
        ('[(63){42}]'::log2_hist)
) AS t(h);

-- =========================================================================
-- 3. Raw 0-Based (0..63) Subscripting & Slicing on log2_hist
-- =========================================================================
WITH sample AS (
    SELECT '[(3){10,20}(1){30}]'::log2_hist AS h
)
SELECT
    h[-1] AS out_of_bounds_low,
    h[0] AS b0,
    h[2] AS b2,
    h[3] AS b3,
    h[4] AS b4,
    h[5] AS b5,
    h[6] AS b6,
    h[7] AS b7,
    h[63] AS b63,
    h[64] AS out_of_bounds_high,
    h[NULL::int] AS null_subscript
FROM sample;

-- Slice subscripting h[lo:hi]
WITH sample AS (
    SELECT '[(1){10,20}(2){30,40}(1){50}]'::log2_hist AS h
)
SELECT
    h[1:2] AS slice_1_2,
    h[2:5] AS slice_2_5,
    h[:2] AS slice_to_2,
    h[5:] AS slice_from_5,
    h[:] AS slice_all,
    h[10:20] AS slice_empty_range,
    h[5:2] AS slice_inverted
FROM sample;

-- =========================================================================
-- 4. 0-Based Live-Range bigint[] Casting & Full 64-Bucket Expansion
-- =========================================================================
SELECT
    ('[]'::log2_hist)::bigint[] AS empty_arr,
    cardinality(('[]'::log2_hist)::bigint[]) AS empty_card;

SELECT
    ('[(0){5,10}]'::log2_hist)::bigint[] AS b0_live_arr,
    array_lower(('[(0){5,10}]'::log2_hist)::bigint[], 1) AS b0_lbound,
    array_upper(('[(0){5,10}]'::log2_hist)::bigint[], 1) AS b0_ubound,
    (('[(0){5,10}]'::log2_hist)::bigint[])[0] AS b0_elem0,
    (('[(0){5,10}]'::log2_hist)::bigint[])[1] AS b0_elem1,
    (('[(0){5,10}]'::log2_hist)::bigint[])[2] AS b0_elem2;

SELECT
    ('[(3){10}(1){20}]'::log2_hist)::bigint[] AS b3_live_arr,
    array_lower(('[(3){10}(1){20}]'::log2_hist)::bigint[], 1) AS b3_lbound,
    array_upper(('[(3){10}(1){20}]'::log2_hist)::bigint[], 1) AS b3_ubound,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])[2] AS b3_elem2,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])[3] AS b3_elem3,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])[4] AS b3_elem4,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])[5] AS b3_elem5,
    (('[(3){10}(1){20}]'::log2_hist)::bigint[])[6] AS b3_elem6;

-- Verify raw h[i] matches (h::bigint[])[i] across the live range
WITH sample AS (
    SELECT '[(3){10}(1){20}]'::log2_hist AS h
)
SELECT
    i,
    h[i] AS raw_val,
    (h::bigint[])[i] AS arr_val
FROM sample, generate_series(2, 6) AS i;

-- Round-trip bijection: (h::bigint[])::log2_hist = h for all histograms
SELECT
    h,
    h::bigint[] AS live_arr,
    (h::bigint[])::log2_hist = h AS roundtrip_ok
FROM (
    VALUES
        ('[]'::log2_hist),
        ('[(0){7}]'::log2_hist),
        ('[(0){5,10}]'::log2_hist),
        ('[(1){10,20}]'::log2_hist),
        ('[(3){10}(1){20}]'::log2_hist),
        ('[(63){99}]'::log2_hist),
        ('[(0){1}(62){2}]'::log2_hist)
) AS t(h);

-- Casting short and offset bigint[] arrays to log2_hist
SELECT
    '{}'::bigint[]::log2_hist AS from_empty_arr,
    '[0:2]={1,2,3}'::bigint[]::log2_hist AS from_0_based_short,
    '[10:12]={7,0,8}'::bigint[]::log2_hist AS from_offset_short,
    '{10,20}'::bigint[]::log2_hist AS from_default_1_based;

-- Full 64-element expansion via log2_hist_buckets() and log2_hist_to_array()
SELECT
    cardinality(log2_hist_buckets('[(3){10}(1){20}]'::log2_hist)) AS full_card,
    array_lower(log2_hist_buckets('[(3){10}(1){20}]'::log2_hist), 1) AS full_0_lbound,
    array_upper(log2_hist_buckets('[(3){10}(1){20}]'::log2_hist), 1) AS full_0_ubound,
    (log2_hist_buckets('[(3){10}(1){20}]'::log2_hist))[3] AS full_b3,
    (log2_hist_buckets('[(3){10}(1){20}]'::log2_hist))[4] AS full_b4,
    (log2_hist_buckets('[(3){10}(1){20}]'::log2_hist))[5] AS full_b5,
    log2_hist_buckets('[(3){10}(1){20}]'::log2_hist)::log2_hist AS roundtrip_from_full_0;

-- 1-based full 64-element array round-trip
SELECT
    array_lower(log2_hist_to_array('[(0){11}(62){22}]'::log2_hist, true, 1), 1) AS full_1_lbound,
    array_upper(log2_hist_to_array('[(0){11}(62){22}]'::log2_hist, true, 1), 1) AS full_1_ubound,
    log2_hist_to_array('[(0){11}(62){22}]'::log2_hist, true, 1)::log2_hist AS roundtrip_from_full_1,
    log2_hist_total_count(array_fill(1::bigint, ARRAY[64])::log2_hist) AS fill_64_total;

-- =========================================================================
-- 5. Observation Addition, Merging & Saturating uint64 Arithmetic
-- =========================================================================
SELECT
    log2_hist_empty() + 0 + 1 + 2 + 3 + 4 + 7 + 8 AS added_seq,
    8 + (0 + log2_hist_empty()) AS added_commuted,
    log2_hist_add('[(0){1}]'::log2_hist, '[(1){2}]'::log2_hist) AS added_hist_alias,
    log2_hist_single(15) AS single_15,
    log2_hist_single(-5, true, 10) AS single_neg_clamped_10,
    log2_hist_add('[(0){2}]'::log2_hist, -10, true, 5) AS add_neg_clamped_5,
    log2_hist_total_count(log2_hist_empty() + 0 + 1 + 2 + 3 + 4 + 7 + 8) AS total_cnt;

-- Inspection & analytics functions: min_bucket, max_bucket, percentile
SELECT
    log2_hist_min_bucket('[]'::log2_hist) AS empty_min,
    log2_hist_max_bucket('[]'::log2_hist) AS empty_max,
    log2_hist_percentile('[]'::log2_hist, 0.5) AS empty_p50,
    log2_hist_min_bucket('[(3){10}(1){20}(4){30}]'::log2_hist) AS min_b,
    log2_hist_max_bucket('[(3){10}(1){20}(4){30}]'::log2_hist) AS max_b,
    log2_hist_percentile('[(0){10}]'::log2_hist, 0.5) AS b0_p50,
    log2_hist_percentile('[(3){10}(1){20}(4){70}]'::log2_hist, 0.0) AS p0,
    log2_hist_percentile('[(3){10}(1){20}(4){70}]'::log2_hist, 0.10) AS p10,
    log2_hist_percentile('[(3){10}(1){20}(4){70}]'::log2_hist, 0.25) AS p25,
    log2_hist_percentile('[(3){10}(1){20}(4){70}]'::log2_hist, 0.50) AS p50,
    log2_hist_percentile('[(3){10}(1){20}(4){70}]'::log2_hist, 1.00) AS p100;

-- Saturating addition at UINT64_MAX (18446744073709551615)
SELECT
    '[(0){18446744073709551610}(5){100}]'::log2_hist +
    '[(0){100}(5){200}]'::log2_hist AS saturated_merge;

-- Verify raw 64-bit counter reinterpretation in bigint[] (-1::bigint == UINT64_MAX)
SELECT
    (('[(0){18446744073709551615}]'::log2_hist)::bigint[])[0] AS uint64_max_as_int8,
    ('[0:0]={-1}'::bigint[])::log2_hist AS int8_neg1_as_uint64_max;

-- =========================================================================
-- 6. B-Tree / Hash Operator Classes, DISTINCT, GROUP BY & Catalog Sanity
-- =========================================================================
-- Verify oprcanmerge and oprcanhash are backed by pg_amop entries (opr_sanity check)
SELECT
    o.oprname,
    o.oprcanmerge,
    o.oprcanhash,
    EXISTS (
        SELECT 1 FROM pg_amop a
        JOIN pg_opclass c ON a.amopfamily = c.opcfamily
        JOIN pg_am m ON c.opcmethod = m.oid
        WHERE a.amopopr = o.oid AND m.amname = 'btree'
    ) AS has_btree_amop,
    EXISTS (
        SELECT 1 FROM pg_amop a
        JOIN pg_opclass c ON a.amopfamily = c.opcfamily
        JOIN pg_am m ON c.opcmethod = m.oid
        WHERE a.amopopr = o.oid AND m.amname = 'hash'
    ) AS has_hash_amop
FROM pg_operator o
WHERE o.oprname = '='
  AND o.oprleft = 'log2_hist'::regtype
  AND o.oprright = 'log2_hist'::regtype;

-- Ordering, comparison operators, DISTINCT, and GROUP BY on log2_hist
SELECT
    '[]'::log2_hist < '[(0){1}]'::log2_hist AS empty_lt_b0,
    '[(0){1}]'::log2_hist < '[(0){2}]'::log2_hist AS b0_1_lt_2,
    '[(1){1}]'::log2_hist < '[(0){1}]'::log2_hist AS b1_lt_b0,
    log2_hist_cmp('[(2){5}]'::log2_hist, '[(2){5}]'::log2_hist) AS cmp_eq,
    log2_hist_hash('[(3){10}(1){20}]'::log2_hist) =
        log2_hist_hash('[(3){10,0,20}]'::log2_hist) AS canonical_hash_eq,
    log2_hist_hash_extended('[(3){10}(1){20}]'::log2_hist, 42) =
        log2_hist_hash_extended('[(3){10,0,20}]'::log2_hist, 42) AS canonical_hash_ext_eq;

SELECT h, count(*) AS cnt
FROM (
    VALUES
        ('[(2){10}]'::log2_hist),
        ('[]'::log2_hist),
        ('[(2){10,0}]'::log2_hist),
        ('[(0){1}]'::log2_hist),
        ('[]'::log2_hist)
) AS v(h)
GROUP BY h
ORDER BY h;

-- =========================================================================
-- 7. Table Storage, Subscript Assignment, Aggregates & Unprivileged Role
-- =========================================================================
CREATE TABLE hist_telemetry (
    id serial PRIMARY KEY,
    grp text NOT NULL,
    h log2_hist NOT NULL
);

CREATE INDEX hist_telemetry_btree_idx ON hist_telemetry USING btree (h);
CREATE INDEX hist_telemetry_hash_idx ON hist_telemetry USING hash (h);

INSERT INTO hist_telemetry (grp, h) VALUES
    ('api', '[]'::log2_hist + 0 + 1 + 2 + 3),
    ('api', '[]'::log2_hist + 1 + 3 + 7 + 1024),
    ('db',  '[(10){50,100}]'::log2_hist);

-- Subscript assignment in UPDATE (modify existing bucket, zero a bucket, add bucket 63)
UPDATE hist_telemetry
SET h[0] = 10,
    h[1] = 0,
    h[63] = 3
WHERE id = 1;

SELECT id, grp, h, h[0] AS b0, h[1] AS b1, h[2] AS b2, h[63] AS b63, log2_hist_total_count(h) AS total
FROM hist_telemetry
ORDER BY id;

-- Aggregate observations with log2_hist_agg and merge with log2_hist_merge_agg
SELECT
    log2_hist_agg(v) AS agg_hist,
    log2_hist_total_count(log2_hist_agg(v)) AS agg_total
FROM (VALUES (0), (1), (2), (3), (4), (7), (8), (15), (1023), (1024), (NULL)) AS t(v);

SELECT
    log2_hist_agg(v, true) AS agg_clamped_hist
FROM (VALUES (-100), (-1), (0), (1), (2)) AS t(v);

SELECT
    grp,
    log2_hist_merge_agg(h) AS merged_grp_hist,
    log2_hist_total_count(log2_hist_merge_agg(h)) AS merged_grp_total
FROM hist_telemetry
GROUP BY grp
ORDER BY grp;

-- Parallel aggregation exercising combinefn / serialfn / deserialfn across multiple runs
CREATE TABLE hist_parallel_samples AS
SELECT
    (i % 10) AS grp,
    (1::bigint << (i % 60)) AS v
FROM generate_series(0, 3999) AS i;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

SELECT
    log2_hist_total_count(log2_hist_agg(v)) AS parallel_total,
    (log2_hist_agg(v))[1] AS parallel_b1,
    (log2_hist_agg(v))[60] AS parallel_b60
FROM hist_parallel_samples;

SELECT
    count(*) AS num_groups,
    sum(log2_hist_total_count(h)) AS sum_group_totals
FROM (
    SELECT grp, log2_hist_agg(v) AS h
    FROM hist_parallel_samples
    GROUP BY grp
) AS s;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
DROP TABLE hist_parallel_samples;

-- Test execution under an unprivileged role
CREATE ROLE regress_log2_hist_user;
GRANT SELECT, INSERT, UPDATE ON hist_telemetry TO regress_log2_hist_user;
SET ROLE regress_log2_hist_user;

SELECT grp, log2_hist_merge_agg(h) FROM hist_telemetry GROUP BY grp ORDER BY grp;
SELECT ('[(2){5,10}]'::log2_hist)[2] AS unpriv_sub;

RESET ROLE;
REVOKE ALL ON hist_telemetry FROM regress_log2_hist_user;
DROP ROLE regress_log2_hist_user;
DROP TABLE hist_telemetry;

-- =========================================================================
-- 8. Error & Boundary Cases
-- =========================================================================
-- Negative value without clamp_negative
SELECT log2_hist_bucket(-1);
SELECT log2_hist_bucket(-1, false);
SELECT '[]'::log2_hist + (-1);
SELECT log2_hist_add('[]'::log2_hist, -1, false, 1);
SELECT log2_hist_add('[]'::log2_hist, 1, false, -5);
SELECT log2_hist_agg(v) FROM (VALUES (1), (-1)) AS t(v);

-- Invalid percentile arguments
SELECT log2_hist_percentile('[(0){1}]'::log2_hist, -0.1);
SELECT log2_hist_percentile('[(0){1}]'::log2_hist, 1.1);
SELECT log2_hist_percentile('[(0){1}]'::log2_hist, 'NaN'::float8);

-- Invalid index_base in log2_hist_to_array
SELECT log2_hist_to_array('[(0){1}]'::log2_hist, true, 2);
SELECT log2_hist_to_array('[(0){1}]'::log2_hist, true, 2147483647);

-- Malformed text representations
SELECT 'invalid'::log2_hist;
SELECT '[1,2,3]'::log2_hist;
SELECT '[(0){}]'::log2_hist;
SELECT '[(0){1,}]'::log2_hist;
SELECT '[(0){-1}]'::log2_hist;
SELECT '[(64){1}]'::log2_hist;
SELECT '[(60){1,2,3,4,5}]'::log2_hist;
SELECT '[(0){18446744073709551616}]'::log2_hist;
SELECT '[(0){1}]extra'::log2_hist;

-- Malformed binary wire payloads
SELECT log2_hist_from_bytea('\x21000000'::bytea);
SELECT log2_hist_from_bytea('\x0101000040010000000000000001'::bytea);

-- Invalid array casts
SELECT '[-1:0]={1,2}'::bigint[]::log2_hist;
SELECT '[60:64]={1,2,3,4,5}'::bigint[]::log2_hist;
SELECT '[1000:1001]={1,2}'::bigint[]::log2_hist;
SELECT '{{1,2},{3,4}}'::bigint[]::log2_hist;
SELECT ARRAY[1::bigint, NULL::bigint]::log2_hist;

-- Invalid subscript assignments
DO $$
DECLARE
    h log2_hist := '[]'::log2_hist;
BEGIN
    h[-1] := 1;
END;
$$;

DO $$
DECLARE
    h log2_hist := '[]'::log2_hist;
BEGIN
    h[64] := 1;
END;
$$;

DO $$
DECLARE
    h log2_hist := '[]'::log2_hist;
BEGIN
    h[NULL] := 1;
END;
$$;

DO $$
DECLARE
    h log2_hist := '[]'::log2_hist;
BEGIN
    h[0] := NULL;
END;
$$;

DROP EXTENSION log2_hist;
