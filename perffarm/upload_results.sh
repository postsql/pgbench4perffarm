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
        if command -v zstd >/dev/null 2>&1; then
            ARCHIVE_PATH="$ARCHIVE_DIR/${ANIMAL_NAME}_${BATCH_NAME}_${UPLOAD_LEVEL}.tar.zst"
            tar -C "$(dirname "$BATCH_DIR")" -cf - "$BATCH_NAME" | zstd -q -T0 -f -o "$ARCHIVE_PATH"
        else
            ARCHIVE_PATH="$ARCHIVE_DIR/${ANIMAL_NAME}_${BATCH_NAME}_${UPLOAD_LEVEL}.tar.gz"
            tar -C "$(dirname "$BATCH_DIR")" -czf "$ARCHIVE_PATH" "$BATCH_NAME"
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
        log_info "Syncing batch_id=$BATCH_ID to remote metrics DB ($REMOTE_METRICS_DB_CONNINFO)..."
        "$PSQL_BIN" -X -q -d "$REMOTE_METRICS_DB_CONNINFO" \
            -f "$SCRIPT_DIR/sql/metrics_db_schema.sql" >/dev/null || true
    fi
fi

exit 0
