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
    t.ended_at
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
        PARTITION BY s.animal_name, s.pg_branch, s.test_profile, s.test_name
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
    END AS verdict
FROM ordered_runs
ORDER BY batch_id DESC, test_name ASC;

CREATE OR REPLACE FUNCTION perffarm_compare_commits(
    p_sha_a text,
    p_sha_b text,
    p_animal text DEFAULT NULL
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
WITH latest_a AS (
    SELECT DISTINCT ON (s.animal_name, s.test_name)
        s.*
    FROM perffarm_commit_summary_v s
    WHERE s.git_commit_sha LIKE (p_sha_a || '%')
      AND (p_animal IS NULL OR s.animal_name = p_animal)
      AND s.tps IS NOT NULL
    ORDER BY s.animal_name, s.test_name, s.batch_id DESC, s.test_run_id DESC
),
latest_b AS (
    SELECT DISTINCT ON (s.animal_name, s.test_name)
        s.*
    FROM perffarm_commit_summary_v s
    WHERE s.git_commit_sha LIKE (p_sha_b || '%')
      AND (p_animal IS NULL OR s.animal_name = p_animal)
      AND s.tps IS NOT NULL
    ORDER BY s.animal_name, s.test_name, s.batch_id DESC, s.test_run_id DESC
),
paired AS (
    SELECT
        a.animal_name,
        a.test_name,
        a.suite_name,
        m.ord,
        m.metric,
        m.unit,
        m.higher_is_better,
        m.val_a,
        m.val_b
    FROM latest_a a
    JOIN latest_b b
      ON b.animal_name = a.animal_name
     AND b.test_name = a.test_name
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
ORDER BY p.animal_name, p.test_name, p.ord;
$func$;
