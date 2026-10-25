#!/usr/bin/env bash
# =============================================================================
# setup_metrics_db.sh — Provision, start, and initialize the Perffarm Metrics
# PostgreSQL Database and register the animal in perffarm_animals.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${CONF_FILE:-$SCRIPT_DIR/conf/animal.conf}"

ACTION="ensure"
PG_CONFIG_OVERRIDE=""

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
        --pg-config=*)
            PG_CONFIG_OVERRIDE="${1#*=}"
            shift
            ;;
        --pg-config)
            PG_CONFIG_OVERRIDE="$2"
            shift 2
            ;;
        --start|--ensure)
            ACTION="ensure"
            shift
            ;;
        --stop)
            ACTION="stop"
            shift
            ;;
        --status)
            ACTION="status"
            shift
            ;;
        --reset)
            ACTION="reset"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --config=PATH      Path to animal.conf (default: perffarm/conf/animal.conf)
  --pg-config=PATH   Path to pg_config binary used to run local metrics DB
  --ensure, --start  Initialize (if needed), start local metrics DB, apply schema & upsert animal (default)
  --stop             Stop local metrics DB cluster
  --status           Show metrics DB status and row counts
  --reset            Stop, wipe, and re-initialize local metrics DB cluster
  -h, --help         Show this help message
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

log_info() {
    echo "[setup_metrics_db] $(date -u '+%Y-%m-%dT%H:%M:%SZ') $*"
}

resolve_pg_bindir() {
    local candidate
    local candidates=(
        "$PG_CONFIG_OVERRIDE"
        "${METRICS_PG_CONFIG:-}"
        "$WORKDIR/metrics_pg_install/bin/pg_config"
        "${PREBUILT_PG_CONFIG:-}"
        "${PG_CONFIG:-}"
        "pg_config"
        "$HOME/pg20_install/bin/pg_config"
        "$HOME/pg18/bin/pg_config"
    )
    for sys_pgc in /usr/lib/postgresql/*/bin/pg_config; do
        if [[ -x "$sys_pgc" ]]; then
            candidates+=("$sys_pgc")
        fi
    done

    for candidate in "${candidates[@]}"; do
        if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1; then
            "$candidate" --bindir
            return 0
        fi
    done

    # Fallback: if only the mutable target build ($WORKDIR/install) exists, copy it
    # into a dedicated $WORKDIR/metrics_pg_install so future multi-commit rebuilds
    # across CATALOG_VERSION_NO bumps do not overwrite the Metrics DB binaries.
    if [[ -x "$WORKDIR/install/bin/pg_config" ]]; then
        if [[ ! -x "$WORKDIR/metrics_pg_install/bin/pg_config" ]]; then
            rm -rf "$WORKDIR/metrics_pg_install"
            cp -a "$WORKDIR/install" "$WORKDIR/metrics_pg_install"
        fi
        echo "$WORKDIR/metrics_pg_install/bin"
        return 0
    fi

    echo "[setup_metrics_db] ERROR: Could not locate pg_config. Pass --pg-config=/path/to/pg_config or set METRICS_PG_CONFIG / PREBUILT_PG_CONFIG." >&2
    return 1
}

PGBIN="$(resolve_pg_bindir)"
INITDB="$PGBIN/initdb"
PG_CTL="$PGBIN/pg_ctl"
PSQL="$PGBIN/psql"
PG_ISREADY="$PGBIN/pg_isready"

METRICS_DB_MODE="${METRICS_DB_MODE:-local}"
METRICS_DB_HOST="${METRICS_DB_HOST:-127.0.0.1}"
METRICS_DB_PORT="${METRICS_DB_PORT:-55433}"
METRICS_DB_NAME="${METRICS_DB_NAME:-perffarm_metrics}"
METRICS_DB_DATADIR="${METRICS_DB_DATADIR:-$WORKDIR/metrics_pgdata}"
METRICS_DB_SCHEMA="${METRICS_DB_SCHEMA:-public}"
METRICS_DB_CONNINFO="${METRICS_DB_CONNINFO:-host=$METRICS_DB_HOST port=$METRICS_DB_PORT dbname=$METRICS_DB_NAME}"
mkdir -p "$LOG_DIR" "$STATE_DIR"
METRICS_LOG="$LOG_DIR/metrics_db.log"

stop_local_cluster() {
    if [[ "$METRICS_DB_MODE" == "local" && -f "$METRICS_DB_DATADIR/PG_VERSION" ]]; then
        if "$PG_CTL" -D "$METRICS_DB_DATADIR" status >/dev/null 2>&1; then
            log_info "Stopping local metrics DB cluster ($METRICS_DB_DATADIR)..."
            "$PG_CTL" -D "$METRICS_DB_DATADIR" -m fast stop
        fi
    fi
}

ensure_local_cluster() {
    if [[ "$METRICS_DB_MODE" != "local" ]]; then
        log_info "METRICS_DB_MODE=$METRICS_DB_MODE; using external METRICS_DB_CONNINFO."
        return 0
    fi

    if [[ ! -f "$METRICS_DB_DATADIR/PG_VERSION" ]]; then
        log_info "Initializing local metrics PostgreSQL cluster at $METRICS_DB_DATADIR (port $METRICS_DB_PORT)..."
        rm -rf "$METRICS_DB_DATADIR"
        mkdir -p "$(dirname "$METRICS_DB_DATADIR")"
        "$INITDB" -D "$METRICS_DB_DATADIR" --encoding=UTF8 --locale=C --no-instructions >/dev/null
        cat >> "$METRICS_DB_DATADIR/postgresql.conf" <<EOF

# --- Perffarm Metrics Database Settings ---
listen_addresses = '*'
port = ${METRICS_DB_PORT}
max_connections = 100
shared_buffers = 256MB
work_mem = 16MB
maintenance_work_mem = 128MB
synchronous_commit = on
checkpoint_completion_target = 0.9
lc_messages = 'C'
timezone = 'UTC'
EOF
        cat >> "$METRICS_DB_DATADIR/pg_hba.conf" <<EOF
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
host    all             all             0.0.0.0/0               trust
host    all             all             ::/0                    trust
EOF
    fi

    if ! "$PG_CTL" -D "$METRICS_DB_DATADIR" status >/dev/null 2>&1; then
        log_info "Starting local metrics DB cluster on port $METRICS_DB_PORT (log: $METRICS_LOG)..."
        "$PG_CTL" -D "$METRICS_DB_DATADIR" -l "$METRICS_LOG" -w start >/dev/null
    fi

    "$PG_ISREADY" -h "$METRICS_DB_HOST" -p "$METRICS_DB_PORT" -q

    # Create target database if not present
    DB_EXISTS=$("$PSQL" -X -q -t -A -h "$METRICS_DB_HOST" -p "$METRICS_DB_PORT" -d postgres \
        -c "SELECT 1 FROM pg_database WHERE datname = '${METRICS_DB_NAME}';" || true)
    if [[ "$DB_EXISTS" != "1" ]]; then
        log_info "Creating database ${METRICS_DB_NAME} on port ${METRICS_DB_PORT}..."
        "$PSQL" -X -q -h "$METRICS_DB_HOST" -p "$METRICS_DB_PORT" -d postgres \
            -c "CREATE DATABASE \"${METRICS_DB_NAME}\";"
    fi
}

apply_schema_and_upsert_animal() {
    log_info "Applying perffarm/sql/metrics_db_schema.sql to $METRICS_DB_CONNINFO (schema=$METRICS_DB_SCHEMA)..."
    if [[ "$METRICS_DB_SCHEMA" != "public" ]]; then
        "$PSQL" -X -q -d "$METRICS_DB_CONNINFO" \
            -c "CREATE SCHEMA IF NOT EXISTS \"${METRICS_DB_SCHEMA}\";"
    fi
    "$PSQL" -X -q -v ON_ERROR_STOP=1 -d "$METRICS_DB_CONNINFO" \
        -c "SET search_path TO \"${METRICS_DB_SCHEMA}\", public, pg_catalog;" \
        -f "$SCRIPT_DIR/sql/metrics_db_schema.sql" >/dev/null

    # Ensure hardware_profile.json exists before upserting perffarm_animals
    if [[ ! -f "$STATE_DIR/hardware_profile.json" ]]; then
        bash "$SCRIPT_DIR/setup_machine.sh" --config="$CONF_FILE"
    fi

    log_info "Registering/updating animal '${ANIMAL_NAME}' in ${METRICS_DB_SCHEMA}.perffarm_animals..."
    python3 - "$PSQL" "$METRICS_DB_CONNINFO" "$METRICS_DB_SCHEMA" "$STATE_DIR" "$PG_CONF_FILE" \
        "$ANIMAL_NAME" "${ANIMAL_OWNER:-}" "${ANIMAL_ORG:-}" "${ANIMAL_LOCATION:-}" \
        "${ANIMAL_DESCRIPTION:-}" "${TOPOLOGY:-local}" <<'PY'
import json
import os
import subprocess
import sys
from pathlib import Path

(
    psql_bin,
    conninfo,
    schema,
    state_dir,
    pg_conf_file,
    animal_name,
    owner,
    org,
    loc,
    desc,
    topology,
) = sys.argv[1:12]

state_p = Path(state_dir)
hw = json.loads((state_p / "hardware_profile.json").read_text(encoding="utf-8")) if (state_p / "hardware_profile.json").exists() else {}
srv = json.loads((state_p / "server_instance_info.json").read_text(encoding="utf-8")) if (state_p / "server_instance_info.json").exists() else hw.get("server_instance_info", {})
cli = json.loads((state_p / "client_instance_info.json").read_text(encoding="utf-8")) if (state_p / "client_instance_info.json").exists() else hw.get("client_instance_info", {})
os_info = srv.get("os", hw.get("os", {}))
pg_conf = Path(pg_conf_file).read_text(encoding="utf-8") if os.path.exists(pg_conf_file) else ""

def lit(s: str) -> str:
    return "'" + str(s).replace("'", "''") + "'"

def jb(obj) -> str:
    return "'" + json.dumps(obj).replace("'", "''") + "'::jsonb"

sql = f"""
SET search_path TO "{schema}", public, pg_catalog;
INSERT INTO perffarm_animals (
    animal_name, owner, organization, location, description, topology,
    hardware_profile, server_instance_info, client_instance_info, os_info, pg_conf_snapshot, updated_at
) VALUES (
    {lit(animal_name)}, {lit(owner)}, {lit(org)}, {lit(loc)}, {lit(desc)}, {lit(topology)},
    {jb(hw)}, {jb(srv)}, {jb(cli)}, {jb(os_info)}, {lit(pg_conf)}, clock_timestamp()
)
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
    updated_at = clock_timestamp();
"""
subprocess.run([psql_bin, "-X", "-q", "-v", "ON_ERROR_STOP=1", "-d", conninfo, "-c", sql], check=True)
PY
    log_info "Animal '${ANIMAL_NAME}' registered in Metrics DB ($METRICS_DB_CONNINFO)."
}

show_status() {
    echo "=== Perffarm Metrics Database Status ==="
    echo "  Mode     : $METRICS_DB_MODE"
    echo "  Conninfo : $METRICS_DB_CONNINFO"
    echo "  Schema   : $METRICS_DB_SCHEMA"
    if [[ "$METRICS_DB_MODE" == "local" ]]; then
        "$PG_CTL" -D "$METRICS_DB_DATADIR" status || true
    fi
    "$PSQL" -X -d "$METRICS_DB_CONNINFO" -c "
        SET search_path TO \"${METRICS_DB_SCHEMA}\", public, pg_catalog;
        SELECT
            (SELECT count(*) FROM perffarm_animals) AS animals,
            (SELECT count(*) FROM perffarm_batches) AS batches,
            (SELECT count(*) FROM perffarm_test_runs) AS perffarm_test_runs,
            (SELECT count(*) FROM pgbench_runs) AS pgbench_runs;
    " || true
}

case "$ACTION" in
    ensure)
        ensure_local_cluster
        apply_schema_and_upsert_animal
        ;;
    stop)
        stop_local_cluster
        ;;
    reset)
        stop_local_cluster
        if [[ "$METRICS_DB_MODE" == "local" ]]; then
            rm -rf "$METRICS_DB_DATADIR"
        fi
        ensure_local_cluster
        apply_schema_and_upsert_animal
        ;;
    status)
        show_status
        ;;
esac
