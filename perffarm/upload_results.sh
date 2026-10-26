#!/usr/bin/env bash
# =============================================================================
# upload_results.sh — Pluggable Test & Batch Result Uploader and Archiver
# for pgbench4perffarm.
#
# Supports upload targets configured in animal.conf (UPLOAD_TARGETS):
#   - metrics_db        : Primary PostgreSQL Metrics DB (verified/committed)
#   - archive           : Compressed .tar.zst (or .tar.gz) bundle in $ARCHIVE_DIR
#   - remote_metrics_db : Replicates animal, batch, pgbench_runs & test_runs to REMOTE_METRICS_DB_CONNINFO
#   - rest              : HTTP POST of test/batch JSON payload to UPLOAD_REST_URL
#   - scp               : Copies archive bundle via scp to UPLOAD_SCP_TARGET
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${CONF_FILE:-$SCRIPT_DIR/conf/animal.conf}"

TEST_DIR=""
BATCH_DIR=""
BATCH_ID=""
UPLOAD_LEVEL_OVERRIDE=""
PSQL_BIN="${PSQL_BIN:-psql}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config=*)
            CONF_FILE="${1#*=}"
            shift
            ;;
        --config)
            CONF_FILE="$2"
            shift 2
            ;;
        --test-dir=*)
            TEST_DIR="${1#*=}"
            shift
            ;;
        --batch-dir=*)
            BATCH_DIR="${1#*=}"
            shift
            ;;
        --batch-id=*)
            BATCH_ID="${1#*=}"
            shift
            ;;
        --upload-level=*)
            UPLOAD_LEVEL_OVERRIDE="${1#*=}"
            shift
            ;;
        --psql-bin=*)
            PSQL_BIN="${1#*=}"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --config=PATH        Path to animal.conf
  --test-dir=DIR       Per-test result directory to upload immediately after a test
  --batch-dir=DIR      Batch result directory to package & upload at end of batch
  --batch-id=ID        Numeric batch_id in perffarm_batches
  --upload-level=LVL   Override UPLOAD_LEVEL (summary|timeseries|full-trace)
  --psql-bin=PATH      Path to psql binary
  -h, --help           Show this help message
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -f "$CONF_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$CONF_FILE"
else
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/conf/animal.conf.example"
fi

if [[ -n "$UPLOAD_LEVEL_OVERRIDE" ]]; then
    UPLOAD_LEVEL="$UPLOAD_LEVEL_OVERRIDE"
fi
UPLOAD_LEVEL="${UPLOAD_LEVEL:-timeseries}"
UPLOAD_TARGETS="${UPLOAD_TARGETS:-metrics_db,archive}"
mkdir -p "$ARCHIVE_DIR"

log_info() {
    echo "[upload_results] $(date -u '+%Y-%m-%dT%H:%M:%SZ') $*"
}

has_target() {
    local needle="$1"
    [[ ",${UPLOAD_TARGETS}," == *",${needle},"* ]]
}

# 1. Per-test immediate upload hook
if [[ -n "$TEST_DIR" && -d "$TEST_DIR" ]]; then
    BUNDLE_JSON="$TEST_DIR/test_result_bundle.json"
    if [[ -f "$BUNDLE_JSON" ]] && has_target "rest" && [[ -n "${UPLOAD_REST_URL:-}" ]]; then
        log_info "POSTing test_result_bundle.json to $UPLOAD_REST_URL..."
        AUTH_ARGS=()
        if [[ -n "${UPLOAD_REST_TOKEN:-}" ]]; then
            AUTH_ARGS=(-H "Authorization: Bearer ${UPLOAD_REST_TOKEN}")
        fi
        curl -fsS -X POST "$UPLOAD_REST_URL" \
            -H "Content-Type: application/json" \
            "${AUTH_ARGS[@]}" \
            --data-binary "@$BUNDLE_JSON" >/dev/null || \
            echo "[upload_results] WARNING: REST upload failed for $BUNDLE_JSON" >&2
    fi
fi

# 2. Batch-level packaging & upload hook
if [[ -n "$BATCH_DIR" && -d "$BATCH_DIR" ]]; then
    BATCH_NAME="$(basename "$BATCH_DIR")"
    ARCHIVE_PATH=""

    if has_target "archive" || has_target "scp"; then
        TAR_EXCLUDES=()
        if [[ "$UPLOAD_LEVEL" == "summary" ]]; then
            TAR_EXCLUDES=(--exclude='*pgbench_log*')
        fi
        if command -v zstd >/dev/null 2>&1; then
            ARCHIVE_PATH="$ARCHIVE_DIR/${ANIMAL_NAME}_${BATCH_NAME}_${UPLOAD_LEVEL}.tar.zst"
            tar -C "$(dirname "$BATCH_DIR")" "${TAR_EXCLUDES[@]}" -cf - "$BATCH_NAME" | zstd -q -T0 -f -o "$ARCHIVE_PATH"
        else
            ARCHIVE_PATH="$ARCHIVE_DIR/${ANIMAL_NAME}_${BATCH_NAME}_${UPLOAD_LEVEL}.tar.gz"
            tar -C "$(dirname "$BATCH_DIR")" "${TAR_EXCLUDES[@]}" -czf "$ARCHIVE_PATH" "$BATCH_NAME"
        fi
        log_info "Created result archive: $ARCHIVE_PATH ($(du -h "$ARCHIVE_PATH" | awk '{print $1}'))"
    fi

    if has_target "scp" && [[ -n "${UPLOAD_SCP_TARGET:-}" && -n "$ARCHIVE_PATH" ]]; then
        log_info "Uploading archive via scp to $UPLOAD_SCP_TARGET..."
        scp -q "$ARCHIVE_PATH" "$UPLOAD_SCP_TARGET" || \
            echo "[upload_results] WARNING: scp upload failed for $ARCHIVE_PATH" >&2
    fi

    if has_target "rest" && [[ -n "${UPLOAD_REST_URL:-}" && -f "$BATCH_DIR/batch_manifest.json" ]]; then
        log_info "POSTing batch_manifest.json to $UPLOAD_REST_URL..."
        AUTH_ARGS=()
        if [[ -n "${UPLOAD_REST_TOKEN:-}" ]]; then
            AUTH_ARGS=(-H "Authorization: Bearer ${UPLOAD_REST_TOKEN}")
        fi
        curl -fsS -X POST "$UPLOAD_REST_URL" \
            -H "Content-Type: application/json" \
            "${AUTH_ARGS[@]}" \
            --data-binary "@$BATCH_DIR/batch_manifest.json" >/dev/null || \
            echo "[upload_results] WARNING: REST batch manifest upload failed" >&2
    fi

    if has_target "remote_metrics_db" && [[ -n "${REMOTE_METRICS_DB_CONNINFO:-}" && -n "$BATCH_ID" ]]; then
        REMOTE_SCHEMA="${REMOTE_METRICS_DB_SCHEMA:-${METRICS_DB_SCHEMA:-public}}"
        LOCAL_SCHEMA="${METRICS_DB_SCHEMA:-public}"
        log_info "Syncing batch_id=$BATCH_ID (level=$UPLOAD_LEVEL) from $METRICS_DB_CONNINFO ($LOCAL_SCHEMA) to remote metrics DB ($REMOTE_METRICS_DB_CONNINFO, schema=$REMOTE_SCHEMA)..."
        HAS_REMOTE_SCHEMA="$("$PSQL_BIN" -X -q -t -A -d "$REMOTE_METRICS_DB_CONNINFO" \
            -c "SELECT to_regclass('\"${REMOTE_SCHEMA}\".perffarm_batches') IS NOT NULL;" 2>/dev/null || echo "f")"
        if [[ "$HAS_REMOTE_SCHEMA" != "t" ]]; then
            if [[ "$REMOTE_SCHEMA" != "public" ]]; then
                "$PSQL_BIN" -X -q -d "$REMOTE_METRICS_DB_CONNINFO" \
                    -c "CREATE SCHEMA IF NOT EXISTS \"${REMOTE_SCHEMA}\";" >/dev/null || true
            fi
            "$PSQL_BIN" -X -q -v ON_ERROR_STOP=1 -d "$REMOTE_METRICS_DB_CONNINFO" \
                -c "SET client_min_messages TO WARNING; SET search_path TO \"${REMOTE_SCHEMA}\", public, pg_catalog;" \
                -f "$SCRIPT_DIR/sql/metrics_db_schema.sql" >/dev/null
        fi

        python3 - "$PSQL_BIN" "$METRICS_DB_CONNINFO" "$LOCAL_SCHEMA" \
            "$REMOTE_METRICS_DB_CONNINFO" "$REMOTE_SCHEMA" "$BATCH_ID" "$ANIMAL_NAME" "$UPLOAD_LEVEL" <<'PY'
import json
import subprocess
import sys

(
    psql_bin,
    local_conn,
    local_schema,
    remote_conn,
    remote_schema,
    batch_id_str,
    animal_name,
    upload_level,
) = sys.argv[1:9]
batch_id = int(batch_id_str)

def query_json(conn: str, sql: str):
    res = subprocess.run(
        [psql_bin, "-X", "-q", "-t", "-A", "-d", conn, "-c", sql],
        capture_output=True,
        text=True,
        check=True,
    )
    out = res.stdout.strip()
    return json.loads(out) if out else []

def exec_remote(sql: str):
    subprocess.run(
        [psql_bin, "-X", "-q", "-v", "ON_ERROR_STOP=1", "-d", remote_conn],
        input=sql,
        text=True,
        check=True,
    )

def jlit(rows) -> str:
    return "'" + json.dumps(rows).replace("'", "''") + "'::jsonb"

def slit(val) -> str:
    if val is None:
        return "NULL"
    return "'" + str(val).replace("'", "''") + "'"

animals = query_json(
    local_conn,
    f'SELECT COALESCE(json_agg(row_to_json(t)), \'[]\'::json) FROM "{local_schema}".perffarm_animals t WHERE animal_name = \'{animal_name}\';',
)
batches = query_json(
    local_conn,
    f'SELECT COALESCE(json_agg(row_to_json(t)), \'[]\'::json) FROM "{local_schema}".perffarm_batches t WHERE batch_id = {batch_id};',
)
test_runs = query_json(
    local_conn,
    f'SELECT COALESCE(json_agg(row_to_json(t) ORDER BY test_run_id), \'[]\'::json) FROM "{local_schema}".perffarm_test_runs t WHERE batch_id = {batch_id};',
)
run_ids = [int(r["pgbench_run_id"]) for r in test_runs if r.get("pgbench_run_id") is not None]
run_ids_csv = ",".join(str(rid) for rid in run_ids)
pgb_runs = (
    query_json(
        local_conn,
        f'SELECT COALESCE(json_agg(row_to_json(t) ORDER BY run_id), \'[]\'::json) FROM "{local_schema}".pgbench_runs t WHERE run_id IN ({run_ids_csv});',
    )
    if run_ids_csv
    else []
)

# Ensure remote sequences are at least at max(id) before allocating any new IDs
exec_remote(f"""
SET search_path TO "{remote_schema}", public, pg_catalog;
SELECT setval(pg_get_serial_sequence('perffarm_batches', 'batch_id'), GREATEST(COALESCE((SELECT max(batch_id) FROM perffarm_batches), 1), 1), true);
SELECT setval(pg_get_serial_sequence('perffarm_test_runs', 'test_run_id'), GREATEST(COALESCE((SELECT max(test_run_id) FROM perffarm_test_runs), 1), 1), true);
SELECT setval(pg_get_serial_sequence('pgbench_runs', 'run_id'), GREATEST(COALESCE((SELECT max(run_id) FROM pgbench_runs), 1), 1), true);
""")

# Resolve remote IDs without colliding with other animals' batches/runs on remote_conn
existing_remote_batches = []
existing_remote_rids = []
existing_remote_trids = []

if batches:
    b0 = batches[0]
    existing_remote_batches = [
        int(x)
        for x in query_json(
            remote_conn,
            f"""
            SELECT COALESCE(json_agg(batch_id ORDER BY batch_id), '[]'::json)
            FROM "{remote_schema}".perffarm_batches
            WHERE animal_name = {slit(b0["animal_name"])}
              AND pg_branch = {slit(b0["pg_branch"])}
              AND git_commit_sha = {slit(b0["git_commit_sha"])}
              AND test_profile = {slit(b0["test_profile"])}
              AND started_at = {slit(b0["started_at"])}::timestamptz;
            """,
        )
        if x is not None
    ]

if existing_remote_batches:
    eb_csv = ",".join(str(x) for x in existing_remote_batches)
    existing_remote_rids.extend(
        int(x)
        for x in query_json(
            remote_conn,
            f"""
            SELECT COALESCE(json_agg(pgbench_run_id ORDER BY test_run_id), '[]'::json)
            FROM "{remote_schema}".perffarm_test_runs
            WHERE batch_id IN ({eb_csv}) AND pgbench_run_id IS NOT NULL;
            """,
        )
        if x is not None
    )
    existing_remote_trids.extend(
        int(x)
        for x in query_json(
            remote_conn,
            f"""
            SELECT COALESCE(json_agg(test_run_id ORDER BY test_run_id), '[]'::json)
            FROM "{remote_schema}".perffarm_test_runs
            WHERE batch_id IN ({eb_csv});
            """,
        )
        if x is not None
    )

run_labels = [r["run_label"] for r in pgb_runs if r.get("run_label")]
if run_labels:
    lbl_csv = ",".join(slit(lbl) for lbl in run_labels)
    for x in query_json(
        remote_conn,
        f"""
        SELECT COALESCE(json_agg(run_id ORDER BY run_id), '[]'::json)
        FROM "{remote_schema}".pgbench_runs
        WHERE run_label IN ({lbl_csv});
        """,
    ):
        if x is not None and int(x) not in existing_remote_rids:
            existing_remote_rids.append(int(x))

if existing_remote_batches:
    remote_batch_id = existing_remote_batches[0]
else:
    remote_batch_id = int(
        query_json(
            remote_conn,
            f"SELECT json_build_array(nextval(pg_get_serial_sequence('\"{remote_schema}\".perffarm_batches', 'batch_id')));",
        )[0]
    )

rid_map = {}
if len(existing_remote_rids) == len(run_ids):
    for local_rid, rem_rid in zip(run_ids, existing_remote_rids):
        rid_map[local_rid] = rem_rid
elif run_ids:
    alloc_rids = [
        int(x)
        for x in query_json(
            remote_conn,
            f"SELECT json_agg(nextval(pg_get_serial_sequence('\"{remote_schema}\".pgbench_runs', 'run_id'))) FROM generate_series(1, {len(run_ids)});",
        )
    ]
    for local_rid, rem_rid in zip(run_ids, alloc_rids):
        rid_map[local_rid] = rem_rid

tr_map = {}
local_trids = [int(tr["test_run_id"]) for tr in test_runs]
if len(existing_remote_trids) == len(local_trids):
    for l_trid, r_trid in zip(local_trids, existing_remote_trids):
        tr_map[l_trid] = r_trid
elif local_trids:
    alloc_trids = [
        int(x)
        for x in query_json(
            remote_conn,
            f"SELECT json_agg(nextval(pg_get_serial_sequence('\"{remote_schema}\".perffarm_test_runs', 'test_run_id'))) FROM generate_series(1, {len(local_trids)});",
        )
    ]
    for l_trid, r_trid in zip(local_trids, alloc_trids):
        tr_map[l_trid] = r_trid

sql_parts = [
    f'SET search_path TO "{remote_schema}", public, pg_catalog;',
    "BEGIN;",
    "SELECT pg_advisory_xact_lock(73733476);",
]

if animals:
    sql_parts.append(f"""
INSERT INTO perffarm_animals
SELECT * FROM jsonb_populate_recordset(NULL::perffarm_animals, {jlit(animals)})
ON CONFLICT (animal_name) DO UPDATE SET
    owner = EXCLUDED.owner,
    organization = EXCLUDED.organization,
    location = EXCLUDED.location,
    description = EXCLUDED.description,
    topology = EXCLUDED.topology,
    hardware_profile = EXCLUDED.hardware_profile,
    server_instance_info = EXCLUDED.server_instance_info,
    client_instance_info = EXCLUDED.client_instance_info,
    os_info = EXCLUDED.os_info,
    pg_conf_snapshot = EXCLUDED.pg_conf_snapshot,
    updated_at = EXCLUDED.updated_at;
""")

del_batch_ids = sorted(set(existing_remote_batches) | {remote_batch_id})
del_batch_csv = ",".join(str(x) for x in del_batch_ids)
del_run_ids = sorted(set(existing_remote_rids) | set(rid_map.values()))
del_run_csv = ",".join(str(x) for x in del_run_ids)

if del_batch_csv:
    sql_parts.append(f"DELETE FROM perffarm_test_runs WHERE batch_id IN ({del_batch_csv});")

if batches:
    for b in batches:
        b["batch_id"] = remote_batch_id
    sql_parts.append(f"""
DELETE FROM perffarm_batches WHERE batch_id IN ({del_batch_csv});
INSERT INTO perffarm_batches
SELECT * FROM jsonb_populate_recordset(NULL::perffarm_batches, {jlit(batches)});
""")

if run_ids_csv:
    if del_run_csv:
        sql_parts.append(f"""
DELETE FROM pgbench_progress_log WHERE run_id IN ({del_run_csv});
DELETE FROM pgbench_agg_log WHERE run_id IN ({del_run_csv});
DELETE FROM pgbench_tx_log WHERE run_id IN ({del_run_csv});
DELETE FROM pgbench_runs WHERE run_id IN ({del_run_csv});
""")
    if pgb_runs:
        for r in pgb_runs:
            r["run_id"] = rid_map[int(r["run_id"])]
        sql_parts.append(f"""
INSERT INTO pgbench_runs
SELECT * FROM jsonb_populate_recordset(NULL::pgbench_runs, {jlit(pgb_runs)});
""")
    child_tables = [
        "pgbench_run_groups",
        "pgbench_run_scripts",
        "pgbench_run_commands",
        "pgbench_run_recalls",
        "pgbench_run_slas",
    ]
    if upload_level in ("timeseries", "full-trace"):
        child_tables.extend(["pgbench_progress_log", "pgbench_agg_log"])
    if upload_level == "full-trace":
        child_tables.append("pgbench_tx_log")

    for tbl in child_tables:
        rows = query_json(
            local_conn,
            f'SELECT COALESCE(json_agg(row_to_json(t)), \'[]\'::json) FROM "{local_schema}".{tbl} t WHERE run_id IN ({run_ids_csv});',
        )
        if rows:
            for r in rows:
                r["run_id"] = rid_map[int(r["run_id"])]
            sql_parts.append(f"""
INSERT INTO {tbl}
SELECT * FROM jsonb_populate_recordset(NULL::{tbl}, {jlit(rows)});
""")

if test_runs:
    for tr in test_runs:
        tr["test_run_id"] = tr_map[int(tr["test_run_id"])]
        tr["batch_id"] = remote_batch_id
        if tr.get("pgbench_run_id") is not None:
            tr["pgbench_run_id"] = rid_map[int(tr["pgbench_run_id"])]
    sql_parts.append(f"""
INSERT INTO perffarm_test_runs
SELECT * FROM jsonb_populate_recordset(NULL::perffarm_test_runs, {jlit(test_runs)});
""")

sql_parts.append("""
SELECT setval(pg_get_serial_sequence('perffarm_batches', 'batch_id'), COALESCE((SELECT max(batch_id) FROM perffarm_batches), 1), true);
SELECT setval(pg_get_serial_sequence('perffarm_test_runs', 'test_run_id'), COALESCE((SELECT max(test_run_id) FROM perffarm_test_runs), 1), true);
SELECT setval(pg_get_serial_sequence('pgbench_runs', 'run_id'), COALESCE((SELECT max(run_id) FROM pgbench_runs), 1), true);
COMMIT;
""")

exec_remote("\n".join(sql_parts))
PY
        log_info "Successfully replicated batch_id=$BATCH_ID to remote metrics DB ($REMOTE_METRICS_DB_CONNINFO, schema=$REMOTE_SCHEMA)."
    fi
fi

exit 0
