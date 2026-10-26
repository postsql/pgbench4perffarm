-- =============================================================================
-- pgbench4perffarm — Metrics Database Schema (metrics_db_schema.sql)
-- =============================================================================
-- Idempotent DDL script provisioning:
--   1. The 9 pgbench --log-db telemetry tables and built-in views
--   2. Perffarm animal, batch, and test-run catalog tables:
--      - perffarm_animals   (with server/client CPU, RAM & separate system/data/WAL disk info)
--      - perffarm_batches   (with git commit, CI, build/check-world & baseline calibration)
--      - perffarm_test_runs (with pgbench_run_id FK, pg_stat_* deltas, OS metrics & 5-point verification)
--   3. Longitudinal reporting views and commit comparison SRFs:
--      - perffarm_commit_summary_v
--      - perffarm_regressions_v
--      - perffarm_compare_commits(sha_a, sha_b, animal_name)
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1. Core pgbench --log-db Tables (Tier A Summary + Tier B/C Stream Tables)
-- -----------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS pgbench_runs (
    run_id               bigserial PRIMARY KEY,
    run_label            text,
    status               text NOT NULL DEFAULT 'running',
    exit_code            integer,
    started_at           timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at             timestamptz,
    host                 text,
    target_db            text,
    scale                integer,
    clients              integer,
    threads              integer,
    duration_sec         double precision,
    transactions_cnt     bigint,
    transactions_failed  bigint,
    expected_rollbacks   bigint,
    transactions_skipped bigint,
    retries              bigint,
    retried              bigint,
    tps                  double precision,
    latency_avg_ms       double precision,
    latency_stddev_ms    double precision,
    latency_min_ms       double precision,
    latency_max_ms       double precision,
    latency_p50_ms       double precision,
    latency_p90_ms       double precision,
    latency_p95_ms       double precision,
    latency_p99_ms       double precision,
    latency_p99_9_ms     double precision,
    lag_avg_ms           double precision,
    lag_p50_ms           double precision,
    lag_p90_ms           double precision,
    lag_p95_ms           double precision,
    lag_p99_ms           double precision,
    ontime_pct           double precision,
    recall_samples       bigint,
    recall_avg           double precision,
    recall_p5            double precision,
    recall_p50           double precision,
    recall_p95           double precision,
    recall_initial_q1    double precision,
    recall_mature_q4     double precision,
    recall_deg_pct       double precision,
    validation_passed    bigint,
    validation_failed    bigint,
    validation_pct       double precision,
    sla_all_passed       boolean,
    log_db_dropped_rows  bigint DEFAULT 0,
    config               jsonb,
    report               jsonb
);

CREATE TABLE IF NOT EXISTS pgbench_run_groups (
    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,
    group_idx            integer NOT NULL,
    group_name           text NOT NULL,
    clients              integer NOT NULL,
    threads              integer NOT NULL,
    sequential           boolean NOT NULL,
    target_rate          double precision,
    transactions_cnt     bigint,
    transactions_failed  bigint,
    expected_rollbacks   bigint,
    tps                  double precision,
    latency_avg_ms       double precision,
    latency_p50_ms       double precision,
    latency_p90_ms       double precision,
    latency_p95_ms       double precision,
    latency_p99_ms       double precision,
    latency_p99_9_ms     double precision,
    recall_samples       bigint,
    recall_avg           double precision,
    recall_p5            double precision,
    recall_p50           double precision,
    PRIMARY KEY (run_id, group_idx)
);

CREATE TABLE IF NOT EXISTS pgbench_run_scripts (
    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,
    script_idx           integer NOT NULL,
    script_name          text NOT NULL,
    builtin_name         text,
    weight               integer NOT NULL,
    transactions_cnt     bigint,
    transactions_failed  bigint,
    expected_rollbacks   bigint,
    tps                  double precision,
    latency_avg_ms       double precision,
    latency_p50_ms       double precision,
    latency_p90_ms       double precision,
    latency_p95_ms       double precision,
    latency_p99_ms       double precision,
    latency_p99_9_ms     double precision,
    recall_samples       bigint,
    recall_avg           double precision,
    recall_p5            double precision,
    recall_p50           double precision,
    PRIMARY KEY (run_id, script_idx)
);

CREATE TABLE IF NOT EXISTS pgbench_run_commands (
    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,
    script_idx           integer NOT NULL,
    cmd_idx              integer NOT NULL,
    cmd_type             text NOT NULL,
    first_line           text,
    cnt                  bigint,
    failed               bigint,
    retries              bigint,
    latency_avg_ms       double precision,
    latency_stddev_ms    double precision,
    latency_min_ms       double precision,
    latency_max_ms       double precision,
    PRIMARY KEY (run_id, script_idx, cmd_idx)
);

CREATE TABLE IF NOT EXISTS pgbench_run_recalls (
    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,
    scope                text NOT NULL,
    metric_name          text NOT NULL,
    samples              bigint,
    avg_recall           double precision,
    stddev_recall        double precision,
    min_recall           double precision,
    max_recall           double precision,
    p5_recall            double precision,
    p50_recall           double precision,
    p95_recall           double precision,
    initial_q1           double precision,
    mature_q4            double precision,
    delta                double precision,
    degradation_pct      double precision,
    PRIMARY KEY (run_id, scope, metric_name)
);

CREATE TABLE IF NOT EXISTS pgbench_run_slas (
    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,
    sla_idx              integer NOT NULL,
    raw_spec             text NOT NULL,
    target_scope         text NOT NULL,
    metric               text NOT NULL,
    op                   text NOT NULL,
    threshold_val        double precision,
    actual_val           double precision,
    passed               boolean NOT NULL,
    PRIMARY KEY (run_id, sla_idx)
);

CREATE UNLOGGED TABLE IF NOT EXISTS pgbench_progress_log (
    run_id               bigint NOT NULL,
    ts_epoch_sec         double precision NOT NULL,
    elapsed_sec          double precision NOT NULL,
    tps                  double precision,
    latency_avg_ms       double precision,
    latency_stddev_ms    double precision,
    lag_avg_ms           double precision,
    failed_cnt           bigint
);

CREATE UNLOGGED TABLE IF NOT EXISTS pgbench_agg_log (
    run_id                  bigint NOT NULL,
    thread_id               integer NOT NULL,
    interval_start_sec      bigint NOT NULL,
    cnt                     bigint NOT NULL,
    latency_sum_us          double precision,
    latency_sum2_us         double precision,
    latency_min_us          double precision,
    latency_max_us          double precision,
    lag_sum_us              double precision,
    lag_sum2_us             double precision,
    lag_min_us              double precision,
    lag_max_us              double precision,
    skipped_cnt             bigint,
    retried_cnt             bigint,
    retries_cnt             bigint,
    serialization_failures  bigint,
    deadlock_failures       bigint,
    other_sql_failures      bigint,
    recall_samples          bigint,
    recall_sum              double precision,
    validation_passed       bigint,
    validation_failed       bigint
);

CREATE UNLOGGED TABLE IF NOT EXISTS pgbench_tx_log (
    run_id               bigint NOT NULL,
    thread_id            integer NOT NULL,
    client_id            integer NOT NULL,
    group_id             integer NOT NULL,
    script_idx           integer NOT NULL,
    tx_cnt               bigint NOT NULL,
    ts_usec              bigint NOT NULL,
    status               text NOT NULL,
    latency_us           double precision,
    lag_us               double precision,
    retries              integer,
    recall               double precision,
    validation_ok        boolean
);

CREATE OR REPLACE VIEW pgbench_runs_v AS
SELECT
    run_id,
    run_label,
    status,
    exit_code,
    started_at,
    ended_at,
    host,
    target_db,
    scale,
    clients,
    threads,
    round(duration_sec::numeric, 3) AS duration_sec,
    transactions_cnt,
    transactions_failed,
    expected_rollbacks,
    round(tps::numeric, 2) AS tps,
    round(latency_avg_ms::numeric, 3) AS latency_avg_ms,
    round(latency_p50_ms::numeric, 3) AS latency_p50_ms,
    round(latency_p95_ms::numeric, 3) AS latency_p95_ms,
    round(latency_p99_ms::numeric, 3) AS latency_p99_ms,
    round(ontime_pct::numeric, 2) AS ontime_pct,
    round(recall_avg::numeric, 4) AS recall_avg,
    round(recall_p5::numeric, 4) AS recall_p5,
    round(recall_deg_pct::numeric, 2) AS recall_deg_pct,
    round(validation_pct::numeric, 2) AS validation_pct,
    sla_all_passed,
    log_db_dropped_rows
FROM pgbench_runs
ORDER BY run_id DESC;

CREATE OR REPLACE VIEW pgbench_live_progress_v AS
SELECT
    p.run_id,
    r.run_label,
    r.status,
    r.target_db,
    to_timestamp(p.ts_epoch_sec) AS progress_at,
    p.ts_epoch_sec,
    round(p.elapsed_sec::numeric, 2) AS elapsed_sec,
    round(p.tps::numeric, 2) AS tps,
    round(p.latency_avg_ms::numeric, 3) AS latency_avg_ms,
    round(p.latency_stddev_ms::numeric, 3) AS latency_stddev_ms,
    round(p.lag_avg_ms::numeric, 3) AS lag_avg_ms,
    p.failed_cnt
FROM pgbench_progress_log p
JOIN pgbench_runs r ON r.run_id = p.run_id
ORDER BY p.run_id DESC, p.elapsed_sec ASC;

CREATE OR REPLACE FUNCTION pgbench_show_run(p_run_id bigint DEFAULT NULL)
RETURNS TABLE (
    section text,
    scope   text,
    metric  text,
    value   text,
    unit    text,
    details text
) LANGUAGE sql STABLE AS $func$
WITH target_run AS (
    SELECT r.*
    FROM pgbench_runs r
    WHERE r.run_id = COALESCE(p_run_id, (SELECT max(r2.run_id) FROM pgbench_runs r2))
),
all_rows AS (
    SELECT 100 AS ord, '1. Run Info'::text AS section, 'run'::text AS scope,
           'run_id'::text AS metric, r.run_id::text AS value, ''::text AS unit,
           COALESCE('label=' || r.run_label, 'unlabeled') AS details
    FROM target_run r
    UNION ALL
    SELECT 101, '1. Run Info', 'run', 'run_label', COALESCE(r.run_label, ''), '', ''
    FROM target_run r
    UNION ALL
    SELECT 102, '1. Run Info', 'run', 'status', r.status, '',
           'exit_code=' || COALESCE(r.exit_code::text, 'NULL')
    FROM target_run r
    UNION ALL
    SELECT 103, '1. Run Info', 'run', 'target_db', COALESCE(r.target_db, ''), '',
           'host=' || COALESCE(r.host, 'local') || ', scale=' || COALESCE(r.scale::text, '1')
    FROM target_run r
    UNION ALL
    SELECT 104, '1. Run Info', 'run', 'concurrency',
           COALESCE(r.clients::text, '0') || ' clients / ' || COALESCE(r.threads::text, '0') || ' threads',
           '', 'started=' || r.started_at::text
    FROM target_run r
    UNION ALL
    SELECT 105, '1. Run Info', 'run', 'duration_sec',
           COALESCE(round(r.duration_sec::numeric, 4)::text, '0'), 's',
           'ended=' || COALESCE(r.ended_at::text, 'running')
    FROM target_run r
    UNION ALL
    SELECT 200, '2. Overall Metrics', 'overall', 'tps',
           COALESCE(round(r.tps::numeric, 4)::text, '0'), 'tx/s',
           'processed=' || COALESCE(r.transactions_cnt::text, '0')
    FROM target_run r
    UNION ALL
    SELECT 201, '2. Overall Metrics', 'overall', 'transactions_cnt',
           COALESCE(r.transactions_cnt::text, '0'), 'tx',
           'failed=' || COALESCE(r.transactions_failed::text, '0') ||
           ', expected_rollbacks=' || COALESCE(r.expected_rollbacks::text, '0') ||
           ', skipped=' || COALESCE(r.transactions_skipped::text, '0')
    FROM target_run r
    UNION ALL
    SELECT 202, '2. Overall Metrics', 'overall', 'latency_avg_ms',
           COALESCE(round(r.latency_avg_ms::numeric, 3)::text, '0'), 'ms',
           'stddev=' || COALESCE(round(r.latency_stddev_ms::numeric, 3)::text, '0') ||
           ' ms, min=' || COALESCE(round(r.latency_min_ms::numeric, 3)::text, '0') ||
           ' ms, max=' || COALESCE(round(r.latency_max_ms::numeric, 3)::text, '0') || ' ms'
    FROM target_run r
    UNION ALL
    SELECT 203, '2. Overall Metrics', 'overall', 'latency_p50_ms',
           COALESCE(round(r.latency_p50_ms::numeric, 3)::text, '0'), 'ms',
           'p90=' || COALESCE(round(r.latency_p90_ms::numeric, 3)::text, '0') || ' ms'
    FROM target_run r
    UNION ALL
    SELECT 204, '2. Overall Metrics', 'overall', 'latency_p95_ms',
           COALESCE(round(r.latency_p95_ms::numeric, 3)::text, '0'), 'ms',
           'p99=' || COALESCE(round(r.latency_p99_ms::numeric, 3)::text, '0') ||
           ' ms, p99.9=' || COALESCE(round(r.latency_p99_9_ms::numeric, 3)::text, '0') || ' ms'
    FROM target_run r
    UNION ALL
    SELECT 205, '2. Overall Metrics', 'overall', 'validation_pct',
           COALESCE(round(r.validation_pct::numeric, 2)::text, '100.00'), '%',
           'passed=' || COALESCE(r.validation_passed::text, '0') ||
           ', failed=' || COALESCE(r.validation_failed::text, '0')
    FROM target_run r
    WHERE COALESCE(r.validation_passed, 0) + COALESCE(r.validation_failed, 0) > 0
    UNION ALL
    SELECT 206, '2. Overall Metrics', 'overall', 'sla_all_passed',
           COALESCE(r.sla_all_passed::text, 'true'), '',
           'dropped_log_rows=' || COALESCE(r.log_db_dropped_rows::text, '0')
    FROM target_run r
    UNION ALL
    SELECT 300 + g.group_idx * 10 + 1, '3. Client Groups',
           'group:' || g.group_name, 'tps',
           COALESCE(round(g.tps::numeric, 4)::text, '0'), 'tx/s',
           'clients=' || g.clients || ', threads=' || g.threads ||
           ', seq=' || g.sequential || ', cnt=' || COALESCE(g.transactions_cnt, 0)
    FROM pgbench_run_groups g
    JOIN target_run r ON r.run_id = g.run_id
    UNION ALL
    SELECT 300 + g.group_idx * 10 + 2, '3. Client Groups',
           'group:' || g.group_name, 'latency_p95_ms',
           COALESCE(round(g.latency_p95_ms::numeric, 3)::text, '0'), 'ms',
           'avg=' || COALESCE(round(g.latency_avg_ms::numeric, 3)::text, '0') ||
           ' ms, p50=' || COALESCE(round(g.latency_p50_ms::numeric, 3)::text, '0') ||
           ' ms, p99=' || COALESCE(round(g.latency_p99_ms::numeric, 3)::text, '0') || ' ms'
    FROM pgbench_run_groups g
    JOIN target_run r ON r.run_id = g.run_id
    UNION ALL
    SELECT 400 + s.script_idx * 10 + 1, '4. Scripts',
           'script:' || s.script_name, 'tps',
           COALESCE(round(s.tps::numeric, 4)::text, '0'), 'tx/s',
           'weight=' || s.weight || ', cnt=' || COALESCE(s.transactions_cnt, 0) ||
           ', failed=' || COALESCE(s.transactions_failed, 0)
    FROM pgbench_run_scripts s
    JOIN target_run r ON r.run_id = s.run_id
    UNION ALL
    SELECT 400 + s.script_idx * 10 + 2, '4. Scripts',
           'script:' || s.script_name, 'latency_p95_ms',
           COALESCE(round(s.latency_p95_ms::numeric, 3)::text, '0'), 'ms',
           'avg=' || COALESCE(round(s.latency_avg_ms::numeric, 3)::text, '0') ||
           ' ms, p50=' || COALESCE(round(s.latency_p50_ms::numeric, 3)::text, '0') ||
           ' ms, p99=' || COALESCE(round(s.latency_p99_ms::numeric, 3)::text, '0') || ' ms'
    FROM pgbench_run_scripts s
    JOIN target_run r ON r.run_id = s.run_id
    UNION ALL
    SELECT 500, '5. Recall & Aging',
           rec.scope || ':' || rec.metric_name, 'avg_recall',
           COALESCE(round(rec.avg_recall::numeric, 4)::text, '0'), 'ratio',
           'samples=' || COALESCE(rec.samples, 0) ||
           ', p5=' || COALESCE(round(rec.p5_recall::numeric, 4)::text, '0') ||
           ', p50=' || COALESCE(round(rec.p50_recall::numeric, 4)::text, '0') ||
           CASE WHEN rec.initial_q1 IS NOT NULL THEN
               ', q1=' || round(rec.initial_q1::numeric, 4)::text ||
               ', q4=' || round(rec.mature_q4::numeric, 4)::text ||
               ', deg_pct=' || round(rec.degradation_pct::numeric, 2)::text || '%'
           ELSE '' END
    FROM pgbench_run_recalls rec
    JOIN target_run r ON r.run_id = rec.run_id
    UNION ALL
    SELECT 600 + c.script_idx * 100 + c.cmd_idx, '6. Commands',
           'script#' || c.script_idx || ':cmd#' || c.cmd_idx, c.cmd_type,
           COALESCE(round(c.latency_avg_ms::numeric, 3)::text, '0'), 'ms',
           COALESCE(c.first_line, '') || ' (cnt=' || COALESCE(c.cnt, 0) ||
           ', failed=' || COALESCE(c.failed, 0) || ', retries=' || COALESCE(c.retries, 0) || ')'
    FROM pgbench_run_commands c
    JOIN target_run r ON r.run_id = c.run_id
    UNION ALL
    SELECT 700 + sla.sla_idx, '7. SLA Verifications',
           sla.target_scope, sla.raw_spec,
           CASE WHEN sla.passed THEN 'PASS' ELSE 'FAIL' END, '',
           'actual=' || COALESCE(round(sla.actual_val::numeric, 4)::text, '0') ||
           ' ' || sla.op || ' threshold=' || COALESCE(round(sla.threshold_val::numeric, 4)::text, '0')
    FROM pgbench_run_slas sla
    JOIN target_run r ON r.run_id = sla.run_id
)
SELECT section, scope, metric, value, unit, details
FROM all_rows
ORDER BY ord, scope, metric;
$func$;

CREATE OR REPLACE FUNCTION pgbench_show_run(p_run_label text)
RETURNS TABLE (
    section text,
    scope   text,
    metric  text,
    value   text,
    unit    text,
    details text
) LANGUAGE sql STABLE AS $func$
SELECT s.section, s.scope, s.metric, s.value, s.unit, s.details
FROM pgbench_show_run(
    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_run_label ORDER BY r.run_id DESC LIMIT 1)
) s
WHERE EXISTS (SELECT 1 FROM pgbench_runs r WHERE r.run_label = p_run_label);
$func$;

CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_base_run_id bigint, p_cand_run_id bigint)
RETURNS TABLE (
    scope      text,
    metric     text,
    unit       text,
    base_val   double precision,
    cand_val   double precision,
    abs_delta  double precision,
    pct_change double precision,
    verdict    text
) LANGUAGE sql STABLE AS $func$
WITH base_r AS (
    SELECT * FROM pgbench_runs WHERE run_id = p_base_run_id
),
cand_r AS (
    SELECT * FROM pgbench_runs WHERE run_id = p_cand_run_id
),
pairs AS (
    SELECT 10 AS ord, 'overall'::text AS scope, 'tps'::text AS metric, 'tx/s'::text AS unit,
           true AS higher_better, b.tps AS base_val, c.tps AS cand_val
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 11, 'overall', 'latency_avg_ms', 'ms', false, b.latency_avg_ms, c.latency_avg_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 12, 'overall', 'latency_p50_ms', 'ms', false, b.latency_p50_ms, c.latency_p50_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 13, 'overall', 'latency_p90_ms', 'ms', false, b.latency_p90_ms, c.latency_p90_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 14, 'overall', 'latency_p95_ms', 'ms', false, b.latency_p95_ms, c.latency_p95_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 15, 'overall', 'latency_p99_ms', 'ms', false, b.latency_p99_ms, c.latency_p99_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 16, 'overall', 'latency_p99_9_ms', 'ms', false, b.latency_p99_9_ms, c.latency_p99_9_ms
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 17, 'overall', 'transactions_failed', 'tx', false,
           b.transactions_failed::double precision, c.transactions_failed::double precision
    FROM base_r b CROSS JOIN cand_r c
    UNION ALL
    SELECT 18, 'overall', 'ontime_pct', '%', true, b.ontime_pct, c.ontime_pct
    FROM base_r b CROSS JOIN cand_r c
    WHERE b.ontime_pct IS NOT NULL OR c.ontime_pct IS NOT NULL
    UNION ALL
    SELECT 19, 'overall', 'recall_avg', 'ratio', true, b.recall_avg, c.recall_avg
    FROM base_r b CROSS JOIN cand_r c
    WHERE COALESCE(b.recall_samples, 0) > 0 OR COALESCE(c.recall_samples, 0) > 0
    UNION ALL
    SELECT 20, 'overall', 'recall_p5', 'ratio', true, b.recall_p5, c.recall_p5
    FROM base_r b CROSS JOIN cand_r c
    WHERE COALESCE(b.recall_samples, 0) > 0 OR COALESCE(c.recall_samples, 0) > 0
    UNION ALL
    SELECT 21, 'overall', 'validation_pct', '%', true, b.validation_pct, c.validation_pct
    FROM base_r b CROSS JOIN cand_r c
    WHERE COALESCE(b.validation_passed, 0) + COALESCE(b.validation_failed, 0) > 0
       OR COALESCE(c.validation_passed, 0) + COALESCE(c.validation_failed, 0) > 0
    UNION ALL
    SELECT 100, 'group:' || bg.group_name, 'tps', 'tx/s', true, bg.tps, cg.tps
    FROM pgbench_run_groups bg
    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id
      AND bg.group_name = cg.group_name
    UNION ALL
    SELECT 101, 'group:' || bg.group_name, 'latency_avg_ms', 'ms', false, bg.latency_avg_ms, cg.latency_avg_ms
    FROM pgbench_run_groups bg
    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id
      AND bg.group_name = cg.group_name
    UNION ALL
    SELECT 102, 'group:' || bg.group_name, 'latency_p95_ms', 'ms', false, bg.latency_p95_ms, cg.latency_p95_ms
    FROM pgbench_run_groups bg
    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id
      AND bg.group_name = cg.group_name
    UNION ALL
    SELECT 200, 'script:' || bs.script_name, 'tps', 'tx/s', true, bs.tps, cs.tps
    FROM pgbench_run_scripts bs
    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id
      AND bs.script_name = cs.script_name
    UNION ALL
    SELECT 201, 'script:' || bs.script_name, 'latency_avg_ms', 'ms', false, bs.latency_avg_ms, cs.latency_avg_ms
    FROM pgbench_run_scripts bs
    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id
      AND bs.script_name = cs.script_name
    UNION ALL
    SELECT 202, 'script:' || bs.script_name, 'latency_p95_ms', 'ms', false, bs.latency_p95_ms, cs.latency_p95_ms
    FROM pgbench_run_scripts bs
    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id
      AND bs.script_name = cs.script_name
    UNION ALL
    SELECT 300, 'recall:' || br.scope || ':' || br.metric_name, 'avg_recall', 'ratio', true,
           br.avg_recall, cr.avg_recall
    FROM pgbench_run_recalls br
    JOIN pgbench_run_recalls cr ON br.run_id = p_base_run_id AND cr.run_id = p_cand_run_id
      AND br.scope = cr.scope AND br.metric_name = cr.metric_name
    UNION ALL
    SELECT 301, 'recall:' || br.scope || ':' || br.metric_name, 'p5_recall', 'ratio', true,
           br.p5_recall, cr.p5_recall
    FROM pgbench_run_recalls br
    JOIN pgbench_run_recalls cr ON br.run_id = p_base_run_id AND cr.run_id = p_cand_run_id
      AND br.scope = cr.scope AND br.metric_name = cr.metric_name
)
SELECT
    p.scope,
    p.metric,
    p.unit,
    p.base_val,
    p.cand_val,
    (p.cand_val - p.base_val) AS abs_delta,
    CASE WHEN p.base_val IS NOT NULL AND abs(p.base_val) > 1e-12
         THEN ((p.cand_val - p.base_val) / abs(p.base_val)) * 100.0
         ELSE 0.0
    END AS pct_change,
    CASE
        WHEN p.base_val IS NULL OR p.cand_val IS NULL THEN 'N/A'
        WHEN abs(p.cand_val - p.base_val) <= 1e-9 THEN 'UNCHANGED'
        WHEN abs(p.base_val) > 1e-12 AND abs(((p.cand_val - p.base_val) / abs(p.base_val)) * 100.0) < 1.0 THEN 'UNCHANGED'
        WHEN p.higher_better AND p.cand_val > p.base_val THEN 'IMPROVED'
        WHEN NOT p.higher_better AND p.cand_val < p.base_val THEN 'IMPROVED'
        ELSE 'REGRESSED'
    END AS verdict
FROM pairs p
ORDER BY p.ord, p.scope, p.metric;
$func$;

CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_base_label text, p_cand_label text)
RETURNS TABLE (
    scope      text,
    metric     text,
    unit       text,
    base_val   double precision,
    cand_val   double precision,
    abs_delta  double precision,
    pct_change double precision,
    verdict    text
) LANGUAGE sql STABLE AS $func$
SELECT c.*
FROM pgbench_compare_runs(
    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_base_label ORDER BY r.run_id DESC LIMIT 1),
    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_cand_label ORDER BY r.run_id DESC LIMIT 1)
) c;
$func$;

CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_run_ids bigint[])
RETURNS TABLE (
    scope          text,
    metric         text,
    unit           text,
    run_id         bigint,
    run_label      text,
    val            double precision,
    delta_vs_base  double precision,
    pct_vs_base    double precision,
    sla_all_passed boolean
) LANGUAGE sql STABLE AS $func$
WITH run_list AS (
    SELECT u.rid, u.ord AS run_ord
    FROM unnest(p_run_ids) WITH ORDINALITY AS u(rid, ord)
),
metrics_per_run AS (
    SELECT 10 AS m_ord, 'overall'::text AS scope, 'tps'::text AS metric, 'tx/s'::text AS unit,
           r.run_id, r.run_label, rl.run_ord, r.tps AS val, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 11, 'overall', 'latency_avg_ms', 'ms', r.run_id, r.run_label, rl.run_ord,
           r.latency_avg_ms, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 12, 'overall', 'latency_p50_ms', 'ms', r.run_id, r.run_label, rl.run_ord,
           r.latency_p50_ms, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 13, 'overall', 'latency_p95_ms', 'ms', r.run_id, r.run_label, rl.run_ord,
           r.latency_p95_ms, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 14, 'overall', 'latency_p99_ms', 'ms', r.run_id, r.run_label, rl.run_ord,
           r.latency_p99_ms, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 15, 'overall', 'transactions_failed', 'tx', r.run_id, r.run_label, rl.run_ord,
           r.transactions_failed::double precision, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    UNION ALL
    SELECT 16, 'overall', 'recall_avg', 'ratio', r.run_id, r.run_label, rl.run_ord,
           r.recall_avg, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    WHERE COALESCE(r.recall_samples, 0) > 0
    UNION ALL
    SELECT 100, 'group:' || g.group_name, 'tps', 'tx/s', r.run_id, r.run_label, rl.run_ord,
           g.tps, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    JOIN pgbench_run_groups g ON g.run_id = r.run_id
    UNION ALL
    SELECT 101, 'group:' || g.group_name, 'latency_p95_ms', 'ms', r.run_id, r.run_label, rl.run_ord,
           g.latency_p95_ms, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    JOIN pgbench_run_groups g ON g.run_id = r.run_id
    UNION ALL
    SELECT 200, 'script:' || s.script_name, 'tps', 'tx/s', r.run_id, r.run_label, rl.run_ord,
           s.tps, r.sla_all_passed
    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid
    JOIN pgbench_run_scripts s ON s.run_id = r.run_id
)
SELECT
    m.scope,
    m.metric,
    m.unit,
    m.run_id,
    m.run_label,
    m.val,
    (m.val - b.val) AS delta_vs_base,
    CASE WHEN b.val IS NOT NULL AND abs(b.val) > 1e-12
         THEN ((m.val - b.val) / abs(b.val)) * 100.0
         ELSE 0.0
    END AS pct_vs_base,
    m.sla_all_passed
FROM metrics_per_run m
LEFT JOIN metrics_per_run b
  ON b.scope = m.scope AND b.metric = m.metric AND b.run_ord = 1
ORDER BY m.m_ord, m.scope, m.metric, m.run_ord;
$func$;

CREATE OR REPLACE FUNCTION pgbench_viewer_payload_json(
    p_mode text DEFAULT 'overview',
    p_run_id bigint DEFAULT NULL,
    p_base_run_id bigint DEFAULT NULL,
    p_cand_run_id bigint DEFAULT NULL,
    p_run_ids bigint[] DEFAULT NULL,
    p_limit int DEFAULT 50
) RETURNS jsonb
LANGUAGE sql STABLE AS $func$
WITH resolved_target AS (
    SELECT COALESCE(
        p_run_id,
        CASE WHEN p_mode = 'live' THEN
            (SELECT r.run_id FROM pgbench_runs r WHERE r.status = 'running' ORDER BY r.run_id DESC LIMIT 1)
        ELSE NULL END,
        (SELECT max(r.run_id) FROM pgbench_runs r)
    ) AS run_id
),
runs_list AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(rv) ORDER BY rv.run_id DESC), '[]'::jsonb) AS data
    FROM (
        SELECT * FROM pgbench_runs_v ORDER BY run_id DESC LIMIT GREATEST(COALESCE(p_limit, 50), 1)
    ) rv
),
active_list AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(ar) ORDER BY ar.run_id DESC), '[]'::jsonb) AS data
    FROM (
        SELECT
            r.run_id,
            r.run_label,
            r.status,
            r.started_at,
            r.host,
            r.target_db,
            r.scale,
            r.clients,
            r.threads,
            round(EXTRACT(EPOCH FROM (clock_timestamp() - r.started_at))::numeric, 1) AS live_elapsed_sec,
            lp.elapsed_sec AS latest_progress_sec,
            round(lp.tps::numeric, 2) AS latest_tps,
            round(lp.latency_avg_ms::numeric, 3) AS latest_latency_avg_ms,
            round(lp.latency_stddev_ms::numeric, 3) AS latest_latency_stddev_ms,
            round(lp.lag_avg_ms::numeric, 3) AS latest_lag_avg_ms,
            lp.failed_cnt AS latest_failed_cnt
        FROM pgbench_runs r
        LEFT JOIN LATERAL (
            SELECT p.elapsed_sec, p.tps, p.latency_avg_ms, p.latency_stddev_ms, p.lag_avg_ms, p.failed_cnt
            FROM pgbench_progress_log p
            WHERE p.run_id = r.run_id
            ORDER BY p.elapsed_sec DESC
            LIMIT 1
        ) lp ON true
        WHERE r.status = 'running'
    ) ar
),
sel_run AS (
    SELECT to_jsonb(r) AS data
    FROM pgbench_runs r
    JOIN resolved_target rt ON rt.run_id = r.run_id
),
show_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(sr)), '[]'::jsonb) AS data
    FROM resolved_target rt
    CROSS JOIN LATERAL pgbench_show_run(rt.run_id) sr
    WHERE rt.run_id IS NOT NULL
),
grp_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(g) ORDER BY g.group_idx), '[]'::jsonb) AS data
    FROM pgbench_run_groups g
    JOIN resolved_target rt ON rt.run_id = g.run_id
),
scr_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(s) ORDER BY s.script_idx), '[]'::jsonb) AS data
    FROM pgbench_run_scripts s
    JOIN resolved_target rt ON rt.run_id = s.run_id
),
cmd_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(c) ORDER BY c.script_idx, c.cmd_idx), '[]'::jsonb) AS data
    FROM pgbench_run_commands c
    JOIN resolved_target rt ON rt.run_id = c.run_id
),
rec_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(rc) ORDER BY rc.scope, rc.metric_name), '[]'::jsonb) AS data
    FROM pgbench_run_recalls rc
    JOIN resolved_target rt ON rt.run_id = rc.run_id
),
sla_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(sl) ORDER BY sl.sla_idx), '[]'::jsonb) AS data
    FROM pgbench_run_slas sl
    JOIN resolved_target rt ON rt.run_id = sl.run_id
),
prog_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC), '[]'::jsonb) AS data
    FROM pgbench_progress_log pl
    JOIN resolved_target rt ON rt.run_id = pl.run_id
),
agg_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(ag) ORDER BY ag.interval_start_sec ASC), '[]'::jsonb) AS data
    FROM (
        SELECT
            a.interval_start_sec,
            min(a.interval_start_sec) OVER () AS first_sec,
            (a.interval_start_sec - min(a.interval_start_sec) OVER ()) AS elapsed_sec,
            sum(a.cnt)::bigint AS cnt,
            CASE WHEN sum(a.cnt) > 0 THEN (sum(a.latency_sum_us) / sum(a.cnt)) / 1000.0 ELSE 0.0 END AS latency_avg_ms,
            max(a.latency_max_us) / 1000.0 AS latency_max_ms,
            CASE WHEN sum(a.cnt) > 0 THEN (sum(a.lag_sum_us) / sum(a.cnt)) / 1000.0 ELSE 0.0 END AS lag_avg_ms,
            sum(a.serialization_failures + a.deadlock_failures + a.other_sql_failures)::bigint AS failed_cnt,
            CASE WHEN sum(a.recall_samples) > 0 THEN sum(a.recall_sum) / sum(a.recall_samples) ELSE NULL END AS recall_avg
        FROM pgbench_agg_log a
        JOIN resolved_target rt ON rt.run_id = a.run_id
        GROUP BY a.interval_start_sec
        ORDER BY a.interval_start_sec ASC
    ) ag
),
tx_rows AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(txb) ORDER BY txb.bucket_sec ASC), '[]'::jsonb) AS data
    FROM (
        SELECT
            (t.ts_usec / 1000000)::bigint AS bucket_sec,
            ((t.ts_usec / 1000000)::bigint - min((t.ts_usec / 1000000)::bigint) OVER ()) AS elapsed_sec,
            count(*) FILTER (WHERE t.status = 'ok')::bigint AS ok_cnt,
            count(*) FILTER (WHERE t.status <> 'ok')::bigint AS err_cnt,
            COALESCE(avg(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0, 0.0) AS latency_avg_ms,
            COALESCE(max(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0, 0.0) AS latency_max_ms,
            avg(t.recall) FILTER (WHERE t.recall IS NOT NULL) AS recall_avg
        FROM pgbench_tx_log t
        JOIN resolved_target rt ON rt.run_id = t.run_id
        GROUP BY (t.ts_usec / 1000000)::bigint
        ORDER BY bucket_sec DESC
        LIMIT 120
    ) txb
),
live_tx_pcts AS (
    SELECT to_jsonb(lt) AS data
    FROM (
        SELECT
            count(*) FILTER (WHERE t.status = 'ok')::bigint AS tx_ok_cnt,
            count(*) FILTER (WHERE t.status <> 'ok')::bigint AS tx_err_cnt,
            round((avg(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS avg_ms,
            round((percentile_cont(0.50) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p50_ms,
            round((percentile_cont(0.90) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p90_ms,
            round((percentile_cont(0.95) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p95_ms,
            round((percentile_cont(0.99) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p99_ms,
            round((percentile_cont(0.999) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p99_9_ms,
            round((avg(t.recall) FILTER (WHERE t.recall IS NOT NULL))::numeric, 4) AS recall_avg
        FROM pgbench_tx_log t
        JOIN resolved_target rt ON rt.run_id = t.run_id
    ) lt
),
pair_diff AS (
    SELECT CASE WHEN p_base_run_id IS NOT NULL AND p_cand_run_id IS NOT NULL THEN
        jsonb_build_object(
            'base_run', (SELECT to_jsonb(r) FROM pgbench_runs r WHERE r.run_id = p_base_run_id),
            'cand_run', (SELECT to_jsonb(r) FROM pgbench_runs r WHERE r.run_id = p_cand_run_id),
            'diff_rows', COALESCE((
                SELECT jsonb_agg(to_jsonb(c))
                FROM pgbench_compare_runs(p_base_run_id, p_cand_run_id) c
            ), '[]'::jsonb),
            'base_progress', COALESCE((
                SELECT jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC)
                FROM pgbench_progress_log pl WHERE pl.run_id = p_base_run_id
            ), '[]'::jsonb),
            'cand_progress', COALESCE((
                SELECT jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC)
                FROM pgbench_progress_log pl WHERE pl.run_id = p_cand_run_id
            ), '[]'::jsonb)
        )
    ELSE NULL END AS data
),
multi_diff AS (
    SELECT CASE WHEN p_run_ids IS NOT NULL AND cardinality(p_run_ids) > 0 THEN
        jsonb_build_object(
            'run_ids', to_jsonb(p_run_ids),
            'selected_runs', COALESCE((
                SELECT jsonb_agg(to_jsonb(r) ORDER BY u.ord)
                FROM unnest(p_run_ids) WITH ORDINALITY AS u(rid, ord)
                JOIN pgbench_runs r ON r.run_id = u.rid
            ), '[]'::jsonb),
            'matrix_rows', COALESCE((
                SELECT jsonb_agg(to_jsonb(m))
                FROM pgbench_compare_runs(p_run_ids) m
            ), '[]'::jsonb)
        )
    ELSE NULL END AS data
)
SELECT jsonb_build_object(
    'schema', current_schema(),
    'generated_at', clock_timestamp(),
    'mode', COALESCE(p_mode, 'overview'),
    'target_run_id', (SELECT run_id FROM resolved_target),
    'runs', (SELECT data FROM runs_list),
    'active_runs', (SELECT data FROM active_list),
    'selected_run', (SELECT data FROM sel_run),
    'show_run_rows', (SELECT data FROM show_rows),
    'groups', (SELECT data FROM grp_rows),
    'scripts', (SELECT data FROM scr_rows),
    'commands', (SELECT data FROM cmd_rows),
    'recalls', (SELECT data FROM rec_rows),
    'slas', (SELECT data FROM sla_rows),
    'progress_series', (SELECT data FROM prog_rows),
    'agg_series', (SELECT data FROM agg_rows),
    'tx_series', (SELECT data FROM tx_rows),
    'live_tx_summary', (SELECT data FROM live_tx_pcts),
    'compare_pair', (SELECT data FROM pair_diff),
    'compare_multi', (SELECT data FROM multi_diff)
);
$func$;

-- -----------------------------------------------------------------------------
-- 2. Perffarm Animal, Batch & Test Run Tables
-- -----------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS perffarm_animals (
    animal_name          text PRIMARY KEY,
    owner                text,
    organization         text,
    location             text,
    description          text,
    topology             text NOT NULL DEFAULT 'local',
    hardware_profile     jsonb NOT NULL DEFAULT '{}'::jsonb,
    server_instance_info jsonb NOT NULL DEFAULT '{}'::jsonb,
    client_instance_info jsonb NOT NULL DEFAULT '{}'::jsonb,
    os_info              jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_conf_snapshot     text,
    created_at           timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at           timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE IF NOT EXISTS perffarm_batches (
    batch_id                 bigserial PRIMARY KEY,
    animal_name              text NOT NULL REFERENCES perffarm_animals(animal_name) ON DELETE CASCADE,
    topology                 text NOT NULL DEFAULT 'local',
    test_profile             text NOT NULL DEFAULT 'standard',
    metrics_level            text NOT NULL DEFAULT 'timeseries',
    pg_branch                text NOT NULL,
    pg_version_num           integer,
    git_commit_sha           text NOT NULL,
    git_commit_ts            timestamptz,
    git_commit_subject       text,
    ci_status                text,
    configure_flags          text,
    cflags                   text,
    build_status             text,
    build_duration_sec       double precision,
    check_world_status       text,
    check_world_duration_sec double precision,
    server_instance_info     jsonb NOT NULL DEFAULT '{}'::jsonb,
    client_instance_info     jsonb NOT NULL DEFAULT '{}'::jsonb,
    baseline_calibration     jsonb NOT NULL DEFAULT '{}'::jsonb,
    tests_planned            integer DEFAULT 0,
    tests_passed             integer DEFAULT 0,
    tests_failed             integer DEFAULT 0,
    tests_skipped            integer DEFAULT 0,
    started_at               timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at                 timestamptz,
    status                   text NOT NULL DEFAULT 'running'
);

CREATE INDEX IF NOT EXISTS perffarm_batches_animal_branch_sha_idx
    ON perffarm_batches (animal_name, pg_branch, git_commit_sha);

CREATE TABLE IF NOT EXISTS perffarm_test_runs (
    test_run_id              bigserial PRIMARY KEY,
    batch_id                 bigint NOT NULL REFERENCES perffarm_batches(batch_id) ON DELETE CASCADE,
    test_name                text NOT NULL,
    suite_name               text NOT NULL,
    pgbench_run_id           bigint REFERENCES pgbench_runs(run_id) ON DELETE SET NULL,
    scale                    integer,
    clients                  integer,
    threads                  integer,
    duration_sec             double precision,
    init_duration_sec        double precision,
    template_reused          boolean NOT NULL DEFAULT false,
    db_size_bytes            bigint,
    server_settings          jsonb NOT NULL DEFAULT '{}'::jsonb,
    baseline_calibration     jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_stat_database_delta   jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_stat_bgwriter_delta   jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_stat_checkpointer_delta jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_stat_wal_delta        jsonb NOT NULL DEFAULT '{}'::jsonb,
    pg_stat_io_delta         jsonb NOT NULL DEFAULT '[]'::jsonb,
    pg_stat_statements_top   jsonb NOT NULL DEFAULT '[]'::jsonb,
    os_metrics               jsonb NOT NULL DEFAULT '{}'::jsonb,
    client_os_metrics        jsonb NOT NULL DEFAULT '{}'::jsonb,
    verification_status      text NOT NULL,
    verification_details     jsonb NOT NULL DEFAULT '{}'::jsonb,
    started_at               timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at                 timestamptz,
    uploaded_at              timestamptz DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS perffarm_test_runs_batch_idx
    ON perffarm_test_runs (batch_id, test_name);
CREATE INDEX IF NOT EXISTS perffarm_test_runs_pgbench_run_idx
    ON perffarm_test_runs (pgbench_run_id);

-- -----------------------------------------------------------------------------
-- 3. Longitudinal Reporting Views & Commit Comparison SRFs
-- -----------------------------------------------------------------------------

CREATE OR REPLACE VIEW perffarm_commit_summary_v AS
SELECT
    t.test_run_id,
    b.batch_id,
    b.animal_name,
    b.topology,
    b.test_profile,
    b.pg_branch,
    b.pg_version_num,
    left(b.git_commit_sha, 12) AS commit_short,
    b.git_commit_sha,
    b.git_commit_ts,
    b.git_commit_subject,
    b.ci_status,
    b.build_status,
    b.check_world_status,
    t.test_name,
    t.suite_name,
    t.verification_status,
    t.pgbench_run_id,
    COALESCE(r.scale, t.scale) AS scale,
    COALESCE(r.clients, t.clients) AS clients,
    COALESCE(r.threads, t.threads) AS threads,
    round(COALESCE(r.duration_sec, t.duration_sec)::numeric, 2) AS duration_sec,
    round(t.init_duration_sec::numeric, 2) AS init_duration_sec,
    t.template_reused,
    round((t.db_size_bytes / (1024.0 * 1024.0))::numeric, 2) AS db_size_mb,
    round(r.tps::numeric, 2) AS tps,
    round(r.latency_avg_ms::numeric, 3) AS latency_avg_ms,
    round(r.latency_p50_ms::numeric, 3) AS latency_p50_ms,
    round(r.latency_p95_ms::numeric, 3) AS latency_p95_ms,
    round(r.latency_p99_ms::numeric, 3) AS latency_p99_ms,
    round(r.recall_avg::numeric, 4) AS recall_avg,
    round(r.recall_p5::numeric, 4) AS recall_p5,
    round(r.recall_deg_pct::numeric, 2) AS recall_deg_pct,
    round(r.validation_pct::numeric, 2) AS validation_pct,
    r.sla_all_passed,
    -- Instance CPU, RAM & Disk Summary (Server & Client)
    COALESCE((b.server_instance_info #>> '{cpu,logical_cpus}')::int, (a.server_instance_info #>> '{cpu,logical_cpus}')::int) AS server_cpus,
    COALESCE((b.server_instance_info #>> '{memory,ram_total_gb}')::numeric, (a.server_instance_info #>> '{memory,ram_total_gb}')::numeric) AS server_ram_gb,
    COALESCE(b.server_instance_info #>> '{disks,system_disk,media_type}', a.server_instance_info #>> '{disks,system_disk,media_type}') AS server_sys_disk_media,
    COALESCE(b.server_instance_info #>> '{disks,data_disk,media_type}', a.server_instance_info #>> '{disks,data_disk,media_type}') AS server_data_disk_media,
    COALESCE(b.server_instance_info #>> '{disks,wal_disk,media_type}', a.server_instance_info #>> '{disks,wal_disk,media_type}') AS server_wal_disk_media,
    COALESCE((b.client_instance_info #>> '{cpu,logical_cpus}')::int, (a.client_instance_info #>> '{cpu,logical_cpus}')::int) AS client_cpus,
    COALESCE((b.client_instance_info #>> '{memory,ram_total_gb}')::numeric, (a.client_instance_info #>> '{memory,ram_total_gb}')::numeric) AS client_ram_gb,
    -- Client-Server Baseline Calibration (SELECT 1 & SELECT repeat('0123456789', 100000))
    round(COALESCE((t.baseline_calibration #>> '{select_1,latency_avg_ms}')::numeric,
                   (b.baseline_calibration #>> '{select_1,latency_avg_ms}')::numeric), 4) AS select1_avg_ms,
    round(COALESCE((t.baseline_calibration #>> '{select_1,qps}')::numeric,
                   (b.baseline_calibration #>> '{select_1,qps}')::numeric), 1) AS select1_qps,
    round(COALESCE((t.baseline_calibration #>> '{select_repeat_1mb,latency_avg_ms}')::numeric,
                   (b.baseline_calibration #>> '{select_repeat_1mb,latency_avg_ms}')::numeric), 3) AS repeat1mb_avg_ms,
    round(COALESCE((t.baseline_calibration #>> '{select_repeat_1mb,throughput_mb_per_sec}')::numeric,
                   (b.baseline_calibration #>> '{select_repeat_1mb,throughput_mb_per_sec}')::numeric), 2) AS repeat1mb_mb_per_sec,
    -- PostgreSQL Catalog Deltas
    round((t.pg_stat_database_delta ->> 'buffer_hit_ratio_pct')::numeric, 2) AS buffer_hit_ratio_pct,
    (t.pg_stat_wal_delta ->> 'wal_bytes')::bigint AS wal_bytes_generated,
    round((t.os_metrics #>> '{cpu_utilization_pct,busy_pct}')::numeric, 1) AS server_cpu_busy_pct,
    t.started_at,
    t.ended_at,
    COALESCE((t.verification_details ->> 'sequence_run_index')::int, 1) AS run_seq_idx,
    COALESCE((t.verification_details ->> 'sequence_total_runs')::int, 1) AS run_seq_total,
    COALESCE((t.verification_details ->> 'target_duration_sec')::int, round(COALESCE(r.duration_sec, t.duration_sec))::int) AS target_duration_sec,
    COALESCE((t.verification_details ->> 'prepared_db_reused')::boolean, false) AS prepared_db_reused,
    r.run_label
FROM perffarm_test_runs t
JOIN perffarm_batches b ON b.batch_id = t.batch_id
JOIN perffarm_animals a ON a.animal_name = b.animal_name
LEFT JOIN pgbench_runs r ON r.run_id = t.pgbench_run_id
ORDER BY b.batch_id DESC, t.test_run_id ASC;

CREATE OR REPLACE VIEW perffarm_regressions_v AS
WITH ordered_runs AS (
    SELECT
        s.animal_name,
        s.pg_branch,
        s.test_profile,
        s.test_name,
        s.suite_name,
        s.batch_id,
        s.test_run_id,
        s.run_seq_idx,
        s.run_seq_total,
        s.target_duration_sec,
        s.run_label,
        s.git_commit_sha,
        s.commit_short,
        s.git_commit_ts,
        s.git_commit_subject,
        s.verification_status,
        s.tps,
        s.latency_p95_ms,
        s.recall_avg,
        LAG(s.commit_short) OVER w AS prev_commit_short,
        LAG(s.tps) OVER w AS prev_tps,
        LAG(s.latency_p95_ms) OVER w AS prev_latency_p95_ms,
        LAG(s.recall_avg) OVER w AS prev_recall_avg
    FROM perffarm_commit_summary_v s
    WHERE s.verification_status IN ('PASSED', 'FAILED')
    WINDOW w AS (
        PARTITION BY s.animal_name, s.pg_branch, s.test_profile, s.test_name, s.run_seq_idx, s.target_duration_sec
        ORDER BY COALESCE(s.git_commit_ts, s.started_at) ASC, s.batch_id ASC, s.test_run_id ASC
    )
)
SELECT
    animal_name,
    pg_branch,
    test_profile,
    test_name,
    suite_name,
    batch_id,
    prev_commit_short,
    commit_short,
    git_commit_subject,
    verification_status,
    prev_tps,
    tps,
    CASE WHEN prev_tps > 0 THEN round(((tps - prev_tps) / prev_tps * 100.0)::numeric, 2) END AS tps_pct_change,
    prev_latency_p95_ms,
    latency_p95_ms,
    CASE WHEN prev_latency_p95_ms > 0 THEN round(((latency_p95_ms - prev_latency_p95_ms) / prev_latency_p95_ms * 100.0)::numeric, 2) END AS latency_p95_pct_change,
    prev_recall_avg,
    recall_avg,
    CASE
        WHEN verification_status = 'FAILED' THEN 'FAILED_VERIFICATION'
        WHEN prev_tps > 0 AND ((tps - prev_tps) / prev_tps * 100.0) < -5.0 THEN 'REGRESSED'
        WHEN prev_latency_p95_ms > 0 AND ((latency_p95_ms - prev_latency_p95_ms) / prev_latency_p95_ms * 100.0) > 10.0 THEN 'REGRESSED'
        WHEN prev_tps > 0 AND ((tps - prev_tps) / prev_tps * 100.0) > 5.0 THEN 'IMPROVED'
        ELSE 'STABLE'
    END AS verdict,
    run_seq_idx,
    target_duration_sec,
    run_label
FROM ordered_runs
ORDER BY batch_id DESC, test_name ASC, run_seq_idx ASC;

-- -----------------------------------------------------------------------------
-- Canonical Test Name Normalizer (Supports Legacy Aliases Across Perffarm)
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION perffarm_canonical_test_name(p_test text)
RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
SELECT CASE p_test
    WHEN 'pgbench-b' THEN 'pgbench-banking'
    WHEN 'pgbench-c' THEN 'pgbench-order'
    WHEN 'pgbench-e' THEN 'pgbench-brokerage'
    WHEN 'pgbench-h' THEN 'pgbench-analytics'
    WHEN 'pgbench-ds' THEN 'pgbench-retail'
    WHEN 'pgbench-graph-lsqb' THEN 'pgbench-graph-subgraph'
    WHEN 'pgbench-graph-snb-interactive' THEN 'pgbench-graph-social-interactive'
    WHEN 'pgbench-graph-snb-bi' THEN 'pgbench-graph-social-bi'
    WHEN 'pgbench-graph-finbench' THEN 'pgbench-graph-financial'
    WHEN 'pgbench-graph-graphalytics' THEN 'pgbench-graph-algorithms'
    WHEN 'pgbench-graph-sql-ud-lsqb' THEN 'pgbench-graph-sql-ud-subgraph'
    WHEN 'pgbench-graph-sql-ud-snb-interactive' THEN 'pgbench-graph-sql-ud-social-interactive'
    WHEN 'pgbench-graph-sql-ud-snb-bi' THEN 'pgbench-graph-sql-ud-social-bi'
    WHEN 'pgbench-graph-sql-ud-finbench' THEN 'pgbench-graph-sql-ud-financial'
    WHEN 'pgbench-graph-sql-ud-graphalytics' THEN 'pgbench-graph-sql-ud-algorithms'
    WHEN 'pgbench-graph-pgq-lsqb' THEN 'pgbench-graph-pgq-subgraph'
    WHEN 'pgbench-graph-pgq-snb-interactive' THEN 'pgbench-graph-pgq-social-interactive'
    WHEN 'pgbench-graph-pgq-snb-bi' THEN 'pgbench-graph-pgq-social-bi'
    WHEN 'pgbench-graph-pgq-finbench' THEN 'pgbench-graph-pgq-financial'
    WHEN 'pgbench-graph-pgq-graphalytics' THEN 'pgbench-graph-pgq-algorithms'
    WHEN 'pgbench-graph-plpgq-lsqb' THEN 'pgbench-graph-plpgq-subgraph'
    WHEN 'pgbench-graph-plpgq-snb-interactive' THEN 'pgbench-graph-plpgq-social-interactive'
    WHEN 'pgbench-graph-plpgq-snb-bi' THEN 'pgbench-graph-plpgq-social-bi'
    WHEN 'pgbench-graph-plpgq-finbench' THEN 'pgbench-graph-plpgq-financial'
    WHEN 'pgbench-graph-plpgq-graphalytics' THEN 'pgbench-graph-plpgq-algorithms'
    ELSE p_test
END;
$$;

DROP FUNCTION IF EXISTS perffarm_compare_commits(text, text, text);
CREATE OR REPLACE FUNCTION perffarm_compare_commits(
    p_sha_a text,
    p_sha_b text,
    p_animal text DEFAULT NULL,
    p_suite_filter text DEFAULT NULL
)
RETURNS TABLE (
    animal_name  text,
    test_name    text,
    suite_name   text,
    metric       text,
    unit         text,
    sha_a_val    double precision,
    sha_b_val    double precision,
    abs_delta    double precision,
    pct_change   double precision,
    verdict      text
) LANGUAGE sql STABLE AS $func$
WITH latest_batch_a AS (
    SELECT DISTINCT ON (s.animal_name, perffarm_canonical_test_name(s.test_name))
        s.animal_name,
        s.test_name,
        perffarm_canonical_test_name(s.test_name) AS canon_test_name,
        s.batch_id
    FROM perffarm_commit_summary_v s
    WHERE s.git_commit_sha LIKE (p_sha_a || '%')
      AND (p_animal IS NULL OR s.animal_name = p_animal)
      AND (p_suite_filter IS NULL OR s.suite_name = p_suite_filter OR s.test_name = p_suite_filter OR perffarm_canonical_test_name(s.test_name) = perffarm_canonical_test_name(p_suite_filter))
      AND s.tps IS NOT NULL
    ORDER BY s.animal_name, perffarm_canonical_test_name(s.test_name), s.batch_id DESC
),
latest_a AS (
    SELECT s.*, lb.canon_test_name
    FROM perffarm_commit_summary_v s
    JOIN latest_batch_a lb
      ON lb.animal_name = s.animal_name
     AND lb.test_name = s.test_name
     AND lb.batch_id = s.batch_id
    WHERE s.tps IS NOT NULL
),
latest_batch_b AS (
    SELECT DISTINCT ON (s.animal_name, perffarm_canonical_test_name(s.test_name))
        s.animal_name,
        s.test_name,
        perffarm_canonical_test_name(s.test_name) AS canon_test_name,
        s.batch_id
    FROM perffarm_commit_summary_v s
    WHERE s.git_commit_sha LIKE (p_sha_b || '%')
      AND (p_animal IS NULL OR s.animal_name = p_animal)
      AND (p_suite_filter IS NULL OR s.suite_name = p_suite_filter OR s.test_name = p_suite_filter OR perffarm_canonical_test_name(s.test_name) = perffarm_canonical_test_name(p_suite_filter))
      AND s.tps IS NOT NULL
    ORDER BY s.animal_name, perffarm_canonical_test_name(s.test_name), s.batch_id DESC
),
latest_b AS (
    SELECT s.*, lb.canon_test_name
    FROM perffarm_commit_summary_v s
    JOIN latest_batch_b lb
      ON lb.animal_name = s.animal_name
     AND lb.test_name = s.test_name
     AND lb.batch_id = s.batch_id
    WHERE s.tps IS NOT NULL
),
paired AS (
    SELECT
        a.animal_name,
        CASE WHEN GREATEST(a.run_seq_total, b.run_seq_total) > 1
             THEN a.canon_test_name || ' [r' || a.run_seq_idx || ' ' || a.target_duration_sec || 's]'
             ELSE a.canon_test_name
        END AS test_name,
        a.suite_name,
        a.run_seq_idx,
        m.ord,
        m.metric,
        m.unit,
        m.higher_is_better,
        m.val_a,
        m.val_b
    FROM latest_a a
    JOIN latest_b b
      ON b.animal_name = a.animal_name
     AND b.canon_test_name = a.canon_test_name
     AND b.run_seq_idx = a.run_seq_idx
     AND b.target_duration_sec = a.target_duration_sec
    CROSS JOIN LATERAL (
        VALUES
            (1, 'tps',                  'tx/s', true,  a.tps::float8,                  b.tps::float8),
            (2, 'latency_avg_ms',       'ms',   false, a.latency_avg_ms::float8,       b.latency_avg_ms::float8),
            (3, 'latency_p50_ms',       'ms',   false, a.latency_p50_ms::float8,       b.latency_p50_ms::float8),
            (4, 'latency_p95_ms',       'ms',   false, a.latency_p95_ms::float8,       b.latency_p95_ms::float8),
            (5, 'latency_p99_ms',       'ms',   false, a.latency_p99_ms::float8,       b.latency_p99_ms::float8),
            (6, 'select1_avg_ms',       'ms',   false, a.select1_avg_ms::float8,       b.select1_avg_ms::float8),
            (7, 'repeat1mb_mb_per_sec', 'MB/s', true,  a.repeat1mb_mb_per_sec::float8, b.repeat1mb_mb_per_sec::float8),
            (8, 'recall_avg',           'ratio',true,  a.recall_avg::float8,           b.recall_avg::float8)
    ) AS m(ord, metric, unit, higher_is_better, val_a, val_b)
    WHERE m.val_a IS NOT NULL AND m.val_b IS NOT NULL
)
SELECT
    p.animal_name,
    p.test_name,
    p.suite_name,
    p.metric,
    p.unit,
    round(p.val_a::numeric, 4)::float8 AS sha_a_val,
    round(p.val_b::numeric, 4)::float8 AS sha_b_val,
    round((p.val_b - p.val_a)::numeric, 4)::float8 AS abs_delta,
    CASE WHEN abs(p.val_a) > 1e-9
         THEN round(((p.val_b - p.val_a) / p.val_a * 100.0)::numeric, 2)::float8
         ELSE 0.0
    END AS pct_change,
    CASE
        WHEN abs(p.val_a) <= 1e-9 THEN 'UNCHANGED'
        WHEN p.higher_is_better AND ((p.val_b - p.val_a) / p.val_a * 100.0) > 2.0 THEN 'IMPROVED'
        WHEN p.higher_is_better AND ((p.val_b - p.val_a) / p.val_a * 100.0) < -2.0 THEN 'REGRESSED'
        WHEN NOT p.higher_is_better AND ((p.val_b - p.val_a) / p.val_a * 100.0) < -2.0 THEN 'IMPROVED'
        WHEN NOT p.higher_is_better AND ((p.val_b - p.val_a) / p.val_a * 100.0) > 2.0 THEN 'REGRESSED'
        ELSE 'UNCHANGED'
    END AS verdict
FROM paired p
ORDER BY p.animal_name, p.run_seq_idx, p.test_name, p.ord;
$func$;
