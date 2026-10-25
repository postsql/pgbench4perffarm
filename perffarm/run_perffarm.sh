#!/usr/bin/env bash
# =============================================================================
# run_perffarm.sh — Regular Perffarm Run Orchestrator for pgbench4perffarm
# =============================================================================
# Executes the complete end-to-end Perffarm workflow:
#   1. Provision/verify server & client instances (local or external Cloud VMs),
#      collect per-instance CPU, RAM, and separate system/data/WAL disk telemetry,
#      and ensure the Metrics Database is online.
#   2. Check out the latest PostgreSQL branch, verify upstream CI status, and
#      deduplicate against previously tested commits in perffarm_batches.
#   3. Configure and build PostgreSQL + pgbench4perffarm (contrib/pgbench &
#      src/bin/pgbench), and verify 'make check-world' / regression & TAP suites.
#   4. Measure client-to-server baseline latency & throughput calibration:
#      - SELECT 1;
#      - SELECT repeat('0123456789', 100000); (1 MB payload transfer)
#   5. For each selected test in test_matrix.json:
#      - Configure & start PostgreSQL server with postgresql.animal.conf + GUC overrides
#      - Verify server readiness & probe required extensions / SQL features
#      - Initialize benchmark dataset (with optional template DB caching)
#      - Capture pre/post pg_stat_* and OS /proc metrics while running pgbench
#        with --log-db and --json-report
#      - Verify the test result (5-point check) & record in perffarm_test_runs
#      - Upload the test result
#   6. Finalize perffarm_batches, create batch archive, upload, and optionally
#      tear down ephemeral Cloud VMs.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONF_FILE="${CONF_FILE:-$SCRIPT_DIR/conf/animal.conf}"

BRANCH_OVERRIDE=""
PROFILE_OVERRIDE=""
TESTS_OVERRIDE=""
METRICS_LEVEL_OVERRIDE=""
UPLOAD_LEVEL_OVERRIDE=""
TOPOLOGY_OVERRIDE=""
SERVER_SSH_OVERRIDE=""
CLIENT_SSH_OVERRIDE=""
PREBUILT_PG_CONFIG_OVERRIDE=""
LOCAL_PG_SRC_OVERRIDE=""
VERIFY_CI_OVERRIDE=""
CHECK_WORLD_OVERRIDE=""
FORCE_RUN=false
NO_TEMPLATE_REUSE=false
SKIP_CALIBRATION=false

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
        --branch=*)
            BRANCH_OVERRIDE="${1#*=}"
            shift
            ;;
        --profile=*)
            PROFILE_OVERRIDE="${1#*=}"
            shift
            ;;
        --tests=*)
            TESTS_OVERRIDE="${1#*=}"
            shift
            ;;
        --metrics-level=*)
            METRICS_LEVEL_OVERRIDE="${1#*=}"
            shift
            ;;
        --upload-level=*)
            UPLOAD_LEVEL_OVERRIDE="${1#*=}"
            shift
            ;;
        --topology=*)
            TOPOLOGY_OVERRIDE="${1#*=}"
            shift
            ;;
        --server-ssh=*)
            SERVER_SSH_OVERRIDE="${1#*=}"
            shift
            ;;
        --client-ssh=*)
            CLIENT_SSH_OVERRIDE="${1#*=}"
            shift
            ;;
        --prebuilt-pg-config=*)
            PREBUILT_PG_CONFIG_OVERRIDE="${1#*=}"
            shift
            ;;
        --local-pg-src=*)
            LOCAL_PG_SRC_OVERRIDE="${1#*=}"
            shift
            ;;
        --verify-ci=*)
            VERIFY_CI_OVERRIDE="${1#*=}"
            shift
            ;;
        --skip-ci-check)
            VERIFY_CI_OVERRIDE="off"
            shift
            ;;
        --check-world=*)
            CHECK_WORLD_OVERRIDE="${1#*=}"
            shift
            ;;
        --fast-check)
            CHECK_WORLD_OVERRIDE="fast"
            shift
            ;;
        --skip-check-world)
            CHECK_WORLD_OVERRIDE="skip"
            shift
            ;;
        --force)
            FORCE_RUN=true
            shift
            ;;
        --no-template-reuse)
            NO_TEMPLATE_REUSE=true
            shift
            ;;
        --skip-calibration)
            SKIP_CALIBRATION=true
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --config=PATH               Path to animal.conf (default: perffarm/conf/animal.conf)
  --branch=NAME               PostgreSQL git branch to test (overrides PG_BRANCHES)
  --profile=TIER              Sizing profile: smoke | standard | nightly (default: from animal.conf)
  --tests=LIST                Comma-separated test or suite names, or 'all'
  --metrics-level=LEVEL       Telemetry granularity: summary | timeseries | full-trace
  --upload-level=LEVEL        Upload granularity: summary | timeseries | full-trace
  --topology=MODE             local | remote-server | remote-client | both-remote
  --server-ssh=USER@HOST      Remote PostgreSQL server SSH target
  --client-ssh=USER@HOST      Remote pgbench client SSH target
  --prebuilt-pg-config=PATH   Use an existing pg_config binary instead of building PostgreSQL from git
  --local-pg-src=DIR          Use an existing local PostgreSQL git checkout
  --verify-ci=MODE            auto | strict | off
  --skip-ci-check             Alias for --verify-ci=off
  --check-world=MODE          full | fast | skip
  --fast-check                Alias for --check-world=fast
  --skip-check-world          Alias for --check-world=skip
  --force                     Run even if this commit was already tested for this animal & profile
  --no-template-reuse         Re-initialize database from scratch for every test
  --skip-calibration          Skip client-server SELECT 1 / repeat('0123456789', 100000) calibration
  -h, --help                  Show this help message
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

[[ -n "$BRANCH_OVERRIDE" ]]             && PG_BRANCHES="$BRANCH_OVERRIDE"
[[ -n "$PROFILE_OVERRIDE" ]]            && TEST_PROFILE="$PROFILE_OVERRIDE"
[[ -n "$TESTS_OVERRIDE" ]]              && TEST_SUITE_FILTER="$TESTS_OVERRIDE"
[[ -n "$METRICS_LEVEL_OVERRIDE" ]]      && METRICS_LEVEL="$METRICS_LEVEL_OVERRIDE"
[[ -n "$UPLOAD_LEVEL_OVERRIDE" ]]       && UPLOAD_LEVEL="$UPLOAD_LEVEL_OVERRIDE"
[[ -n "$TOPOLOGY_OVERRIDE" ]]           && TOPOLOGY="$TOPOLOGY_OVERRIDE"
[[ -n "$SERVER_SSH_OVERRIDE" ]]         && SERVER_SSH_TARGET="$SERVER_SSH_OVERRIDE"
[[ -n "$CLIENT_SSH_OVERRIDE" ]]         && CLIENT_SSH_TARGET="$CLIENT_SSH_OVERRIDE"
[[ -n "$PREBUILT_PG_CONFIG_OVERRIDE" ]] && PREBUILT_PG_CONFIG="$PREBUILT_PG_CONFIG_OVERRIDE"
[[ -n "$LOCAL_PG_SRC_OVERRIDE" ]]       && PG_LOCAL_REPO="$LOCAL_PG_SRC_OVERRIDE"
[[ -n "$VERIFY_CI_OVERRIDE" ]]          && VERIFY_CI="$VERIFY_CI_OVERRIDE"
[[ -n "$CHECK_WORLD_OVERRIDE" ]]        && CHECK_WORLD_MODE="$CHECK_WORLD_OVERRIDE"
[[ "$NO_TEMPLATE_REUSE" == "true" ]]    && REUSE_TEMPLATE_DB="false"
[[ "$SKIP_CALIBRATION" == "true" ]]     && RUN_BASELINE_CALIBRATION="false"

TEST_MATRIX_FILE="${TEST_MATRIX_FILE:-$SCRIPT_DIR/conf/test_matrix.json}"
METRICS_DB_SCHEMA="${METRICS_DB_SCHEMA:-public}"

log_info() {
    echo "[run_perffarm] $(date -u '+%Y-%m-%dT%H:%M:%SZ') $*"
}

# Helper to execute a command on the PostgreSQL server instance (local or remote SSH)
run_on_server() {
    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        # shellcheck disable=SC2086
        ssh $SERVER_SSH_OPTS "$SERVER_SSH_TARGET" "$@"
    else
        bash -c "$*"
    fi
}

# Helper to execute a command on the pgbench client instance (local or remote SSH)
run_on_client() {
    if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
        # shellcheck disable=SC2086
        ssh $CLIENT_SSH_OPTS "$CLIENT_SSH_TARGET" "$@"
    else
        bash -c "$*"
    fi
}

# -----------------------------------------------------------------------------
# Step 0: Provision Hosts (if dynamic), Refresh Hardware Info & Start Metrics DB
# -----------------------------------------------------------------------------
if [[ "${AUTO_PROVISION:-false}" == "true" ]]; then
    bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" provision
fi

bash "$SCRIPT_DIR/setup_machine.sh" --config="$CONF_FILE"
if [[ "$TOPOLOGY" != "local" ]]; then
    bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" sync-repo
fi

PG_CONFIG_FOR_METRICS="${PREBUILT_PG_CONFIG:-${PG_CONFIG:-}}"
bash "$SCRIPT_DIR/setup_metrics_db.sh" --config="$CONF_FILE" \
    ${PG_CONFIG_FOR_METRICS:+--pg-config="$PG_CONFIG_FOR_METRICS"} --ensure

# -----------------------------------------------------------------------------
# Step 1: Checkout Source Tree & Verify CI Status
# -----------------------------------------------------------------------------
FIRST_BRANCH="$(echo "$PG_BRANCHES" | tr ',' ' ' | awk '{print $1}')"
PG_SRC_DIR="$WORKDIR/src/postgres"

GIT_SHA=""
GIT_TS=""
GIT_SUBJECT=""
CI_STATUS="not_checked"

if [[ -n "${PG_LOCAL_REPO:-}" && -d "${PG_LOCAL_REPO}/.git" ]]; then
    PG_SRC_DIR="$PG_LOCAL_REPO"
    GIT_SHA="$(git -C "$PG_SRC_DIR" rev-parse HEAD)"
    GIT_TS="$(git -C "$PG_SRC_DIR" show -s --format=%cI HEAD)"
    GIT_SUBJECT="$(git -C "$PG_SRC_DIR" show -s --format=%s HEAD)"
    FIRST_BRANCH="$(git -C "$PG_SRC_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "$FIRST_BRANCH")"
    log_info "Using local PostgreSQL git tree at $PG_SRC_DIR ($FIRST_BRANCH @ ${GIT_SHA:0:12})"
elif [[ -n "${PREBUILT_PG_CONFIG:-}" ]] && command -v "${PREBUILT_PG_CONFIG}" >/dev/null 2>&1; then
    # Check if there is an associated source tree or fallback to repo commit + pg_config version
    if [[ -d "$PG_SRC_DIR/.git" ]]; then
        REF_GIT="$PG_SRC_DIR"
        GIT_SHA="$(git -C "$REF_GIT" rev-parse HEAD)"
        GIT_TS="$(git -C "$REF_GIT" show -s --format=%cI HEAD)"
        GIT_SUBJECT="$(git -C "$REF_GIT" show -s --format=%s HEAD)"
    else
        GIT_SHA="$(git -C "$REPO_ROOT" rev-parse HEAD)"
        GIT_TS="$(git -C "$REPO_ROOT" show -s --format=%cI HEAD)"
        GIT_SUBJECT="Prebuilt $("$PREBUILT_PG_CONFIG" --version) + pgbench4perffarm $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
    fi
    log_info "Using prebuilt pg_config at $PREBUILT_PG_CONFIG (commit ${GIT_SHA:0:12})"
else
    log_info "Checking out PostgreSQL branch '$FIRST_BRANCH' from $PG_GIT_URL..."
    if [[ ! -d "$PG_SRC_DIR/.git" ]]; then
        git clone --branch "$FIRST_BRANCH" "$PG_GIT_URL" "$PG_SRC_DIR"
    else
        git -C "$PG_SRC_DIR" fetch origin "$FIRST_BRANCH"
        git -C "$PG_SRC_DIR" checkout -f "origin/$FIRST_BRANCH"
    fi
    GIT_SHA="$(git -C "$PG_SRC_DIR" rev-parse HEAD)"
    GIT_TS="$(git -C "$PG_SRC_DIR" show -s --format=%cI HEAD)"
    GIT_SUBJECT="$(git -C "$PG_SRC_DIR" show -s --format=%s HEAD)"
fi

# Verify Upstream CI Status (auto | strict | off)
if [[ "$VERIFY_CI" == "off" ]]; then
    CI_STATUS="skipped"
else
    log_info "Verifying CI status for commit ${GIT_SHA:0:12} (mode=$VERIFY_CI)..."
    CI_STATUS="$(python3 - "$GIT_SHA" "$VERIFY_CI" <<'PY'
import json
import sys
import urllib.request

sha, mode = sys.argv[1], sys.argv[2]
url = f"https://api.github.com/repos/postgres/postgres/commits/{sha}/status"
try:
    req = urllib.request.Request(url, headers={"User-Agent": "pgbench4perffarm/1.0"})
    with urllib.request.urlopen(req, timeout=8) as resp:
        data = json.loads(resp.read().decode("utf-8"))
        state = data.get("state", "unknown")
        if state in ("success", "pending"):
            print(f"github_{state}")
        else:
            print(f"github_{state}")
except Exception:
    print("local_or_unverified")
PY
)"
    if [[ "$VERIFY_CI" == "strict" && "$CI_STATUS" != "github_success" ]]; then
        echo "[run_perffarm] ERROR: Strict CI check failed for $GIT_SHA (status=$CI_STATUS)" >&2
        exit 1
    fi
fi

# Resolve Metrics DB psql binary
resolve_metrics_psql() {
    local candidate bindir
    for candidate in "${PREBUILT_PG_CONFIG:-}" "$WORKDIR/install/bin/pg_config" "${PG_CONFIG:-}" "pg_config"; do
        if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1; then
            bindir="$("$candidate" --bindir 2>/dev/null || true)"
            if [[ -n "$bindir" && -x "$bindir/psql" ]]; then
                echo "$bindir/psql"
                return 0
            fi
        fi
    done
    if [[ -x "$WORKDIR/install/bin/psql" ]]; then
        echo "$WORKDIR/install/bin/psql"
        return 0
    fi
    echo "psql"
}
METRICS_PSQL="$(resolve_metrics_psql)"

# Check commit deduplication in perffarm_batches unless --force
if [[ "$FORCE_RUN" != "true" ]]; then
    ALREADY_TESTED="$("$METRICS_PSQL" -X -q -t -A -d "$METRICS_DB_CONNINFO" -c "
        SELECT count(*)
        FROM \"${METRICS_DB_SCHEMA}\".perffarm_batches
        WHERE animal_name = '${ANIMAL_NAME}'
          AND pg_branch = '${FIRST_BRANCH}'
          AND git_commit_sha = '${GIT_SHA}'
          AND test_profile = '${TEST_PROFILE}'
          AND status = 'completed';
    " || echo "0")"
    if [[ "${ALREADY_TESTED:-0}" -gt 0 ]]; then
        log_info "Commit ${GIT_SHA:0:12} on branch '$FIRST_BRANCH' (profile=$TEST_PROFILE) was already tested on animal '$ANIMAL_NAME'. Pass --force to re-run."
        exit 0
    fi
fi

# -----------------------------------------------------------------------------
# Step 2: Configure & Build PostgreSQL + pgbench4perffarm
# -----------------------------------------------------------------------------
BUILD_START_TS="$(date +%s)"
EFFECTIVE_CFG_FLAGS="$(cat "$STATE_DIR/effective_configure_flags.txt" 2>/dev/null || echo "${CONFIGURE_FLAGS:-}")"

if [[ -n "${PREBUILT_PG_CONFIG:-}" ]] && command -v "${PREBUILT_PG_CONFIG}" >/dev/null 2>&1; then
    TARGET_PG_CONFIG="$(command -v "$PREBUILT_PG_CONFIG")"
    BUILD_STATUS="prebuilt"
else
    log_info "Configuring and building PostgreSQL in $PG_SRC_DIR..."
    (
        cd "$PG_SRC_DIR"
        # shellcheck disable=SC2086
        ./configure --prefix="$WORKDIR/install" CC="${CC:-gcc}" CFLAGS="${CFLAGS:--O2 -g -fno-omit-frame-pointer}" $EFFECTIVE_CFG_FLAGS >"$LOG_DIR/pg_configure.log" 2>&1
        make -j"$BUILD_JOBS" >"$LOG_DIR/pg_build.log" 2>&1
        make install >"$LOG_DIR/pg_install.log" 2>&1
        if [[ -d "contrib/pg_stat_statements" ]]; then
            make -C contrib/pg_stat_statements -j"$BUILD_JOBS" install >>"$LOG_DIR/pg_install.log" 2>&1 || true
        fi
    )
    TARGET_PG_CONFIG="$WORKDIR/install/bin/pg_config"
    BUILD_STATUS="passed"
fi

log_info "Building and installing pgbench4perffarm (contrib/pgbench + src/bin/pgbench) against $TARGET_PG_CONFIG..."
make -C "$REPO_ROOT" -j"$BUILD_JOBS" PG_CONFIG="$TARGET_PG_CONFIG" all >"$LOG_DIR/pgbench4perffarm_build.log" 2>&1
make -C "$REPO_ROOT" PG_CONFIG="$TARGET_PG_CONFIG" install >>"$LOG_DIR/pgbench4perffarm_build.log" 2>&1

BUILD_DURATION_SEC=$(( $(date +%s) - BUILD_START_TS ))

TARGET_BINDIR="$("$TARGET_PG_CONFIG" --bindir)"
PG_INITDB="$TARGET_BINDIR/initdb"
PG_CTL="$TARGET_BINDIR/pg_ctl"
PG_PSQL="$TARGET_BINDIR/psql"
PG_ISREADY="$TARGET_BINDIR/pg_isready"
PGBENCH_BIN="$TARGET_BINDIR/pgbench"
if [[ "$METRICS_PSQL" == "psql" && -x "$PG_PSQL" ]]; then
    METRICS_PSQL="$PG_PSQL"
fi
PG_VER_NUM="$("$TARGET_PG_CONFIG" --version | awk '{print $2}' | sed -E 's/^([0-9]+).*/\10000/')"

# Re-generate postgresql.animal.conf matching exact major version of target server
PG_MAJOR="$(echo "$PG_VER_NUM" | sed -E 's/0000$//')"
python3 "$SCRIPT_DIR/generate_pgconf.py" \
    --hardware-profile="$STATE_DIR/hardware_profile.json" \
    --animal-name="$ANIMAL_NAME" \
    --preset="${PG_SIZING_PRESET:-auto}" \
    --pg-version="$PG_MAJOR" \
    --pg-config="$TARGET_PG_CONFIG" \
    --max-connections="${PG_MAX_CONNECTIONS:-200}" \
    --extra-gucs="${PG_EXTRA_GUCS:-}" \
    --output="$PG_CONF_FILE"

# -----------------------------------------------------------------------------
# Step 3: Verify 'make check-world' / Extension & TAP Regression Suite
# -----------------------------------------------------------------------------
CW_START_TS="$(date +%s)"
CHECK_WORLD_STATUS="skipped"

stop_existing_bench_server() {
    if [[ -f "$PGDATA_DIR/postmaster.pid" ]]; then
        run_on_server "'$PG_CTL' -D '$PGDATA_DIR' -m fast stop" >/dev/null 2>&1 || true
    fi
}

if [[ "$CHECK_WORLD_MODE" == "full" ]]; then
    log_info "Running full 'make check-world' and pgbench4perffarm verification..."
    CW_OK=true
    if [[ -d "$PG_SRC_DIR" && -f "$PG_SRC_DIR/GNUmakefile" && "$BUILD_STATUS" != "prebuilt" ]]; then
        make -C "$PG_SRC_DIR" -j"${CHECK_WORLD_JOBS:-$BUILD_JOBS}" check-world >"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    fi
    stop_existing_bench_server
    rm -rf "$PGDATA_DIR" "$PGWAL_DIR"
    mkdir -p "$(dirname "$PGDATA_DIR")" "$(dirname "$PGWAL_DIR")"
    "$PG_INITDB" -D "$PGDATA_DIR" --waldir="$PGWAL_DIR" --encoding=UTF8 --locale=C --no-instructions >/dev/null
    echo "port = $PGPORT" >> "$PGDATA_DIR/postgresql.conf"
    "$PG_CTL" -D "$PGDATA_DIR" -l "$LOG_DIR/check_server.log" -w start >/dev/null
    make -C "$REPO_ROOT/contrib/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" PGPORT="$PGPORT" PGHOST="127.0.0.1" installcheck >>"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    make -C "$REPO_ROOT/src/bin/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" tapcheck >>"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    "$PG_CTL" -D "$PGDATA_DIR" -m fast stop >/dev/null
    CHECK_WORLD_STATUS="$( $CW_OK && echo "passed" || echo "failed" )"
elif [[ "$CHECK_WORLD_MODE" == "fast" ]]; then
    log_info "Running fast verification (contrib/pgbench SQL regression + pgbench TAP check)..."
    CW_OK=true
    stop_existing_bench_server
    rm -rf "$PGDATA_DIR" "$PGWAL_DIR"
    mkdir -p "$(dirname "$PGDATA_DIR")" "$(dirname "$PGWAL_DIR")"
    "$PG_INITDB" -D "$PGDATA_DIR" --waldir="$PGWAL_DIR" --encoding=UTF8 --locale=C --no-instructions >/dev/null
    echo "port = $PGPORT" >> "$PGDATA_DIR/postgresql.conf"
    "$PG_CTL" -D "$PGDATA_DIR" -l "$LOG_DIR/check_server.log" -w start >/dev/null
    make -C "$REPO_ROOT/contrib/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" PGPORT="$PGPORT" PGHOST="127.0.0.1" installcheck >"$LOG_DIR/fast_check.log" 2>&1 || CW_OK=false
    PROVE_TESTS="t/002_pgbench_no_server.pl" make -C "$REPO_ROOT/src/bin/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" installcheck >>"$LOG_DIR/fast_check.log" 2>&1 || CW_OK=false
    "$PG_CTL" -D "$PGDATA_DIR" -m fast stop >/dev/null
    CHECK_WORLD_STATUS="$( $CW_OK && echo "passed" || echo "failed" )"
    log_info "Fast verification status: $CHECK_WORLD_STATUS"
fi
CW_DURATION_SEC=$(( $(date +%s) - CW_START_TS ))

# Select tests to execute from test_matrix.json
SELECTED_TESTS_JSON="$(python3 - "$TEST_MATRIX_FILE" "$TEST_PROFILE" "$TEST_SUITE_FILTER" <<'PY'
import json
import sys

matrix_file, profile, suite_filter = sys.argv[1:4]
matrix = json.loads(open(matrix_file, encoding="utf-8").read())
filters = [x.strip() for x in suite_filter.split(",") if x.strip()]
allow_all = ("all" in filters) or (not filters)

selected = []
for t in matrix.get("tests", []):
    if not t.get("enabled", True):
        continue
    if not allow_all:
        if (
            t["name"] not in filters
            and t.get("suite") not in filters
            and t.get("category") not in filters
        ):
            continue
    prof_cfg = t.get("profiles", {}).get(profile) or t.get("profiles", {}).get("standard", {})
    selected.append({**t, "effective_profile": prof_cfg})

print(json.dumps(selected))
PY
)"

TESTS_PLANNED="$(python3 -c 'import json,sys; print(len(json.loads(sys.argv[1])))' "$SELECTED_TESTS_JSON")"
log_info "Selected $TESTS_PLANNED test(s) for profile='$TEST_PROFILE' (filter='$TEST_SUITE_FILTER')."

# Create batch record in perffarm_batches
BATCH_ID="$(python3 - "$METRICS_PSQL" "$METRICS_DB_CONNINFO" "$METRICS_DB_SCHEMA" \
    "$ANIMAL_NAME" "$TOPOLOGY" "$TEST_PROFILE" "$METRICS_LEVEL" "$FIRST_BRANCH" "$PG_VER_NUM" \
    "$GIT_SHA" "$GIT_TS" "$GIT_SUBJECT" "$CI_STATUS" "$EFFECTIVE_CFG_FLAGS" "$CFLAGS" \
    "$BUILD_STATUS" "$BUILD_DURATION_SEC" "$CHECK_WORLD_STATUS" "$CW_DURATION_SEC" \
    "$STATE_DIR/server_instance_info.json" "$STATE_DIR/client_instance_info.json" "$TESTS_PLANNED" <<'PY'
import json
import os
import subprocess
import sys

(
    psql_bin,
    conninfo,
    schema,
    animal,
    topology,
    profile,
    mlevel,
    branch,
    ver_num,
    sha,
    commit_ts,
    subject,
    ci_status,
    cfg_flags,
    cflags,
    b_status,
    b_dur,
    cw_status,
    cw_dur,
    srv_path,
    cli_path,
    planned,
) = sys.argv[1:23]

srv = json.loads(open(srv_path, encoding="utf-8").read()) if os.path.exists(srv_path) else {}
cli = json.loads(open(cli_path, encoding="utf-8").read()) if os.path.exists(cli_path) else {}

def lit(v):
    if v is None or v == "":
        return "NULL"
    return "'" + str(v).replace("'", "''") + "'"

def jb(o):
    return "'" + json.dumps(o).replace("'", "''") + "'::jsonb"

sql = f"""
SET search_path TO "{schema}", public, pg_catalog;
INSERT INTO perffarm_batches (
    animal_name, topology, test_profile, metrics_level,
    pg_branch, pg_version_num, git_commit_sha, git_commit_ts, git_commit_subject,
    ci_status, configure_flags, cflags,
    build_status, build_duration_sec, check_world_status, check_world_duration_sec,
    server_instance_info, client_instance_info, tests_planned, status
) VALUES (
    {lit(animal)}, {lit(topology)}, {lit(profile)}, {lit(mlevel)},
    {lit(branch)}, {int(ver_num)}, {lit(sha)}, {lit(commit_ts)}::timestamptz, {lit(subject)},
    {lit(ci_status)}, {lit(cfg_flags)}, {lit(cflags)},
    {lit(b_status)}, {float(b_dur)}, {lit(cw_status)}, {float(cw_dur)},
    {jb(srv)}, {jb(cli)}, {int(planned)}, 'running'
) RETURNING batch_id;
"""
res = subprocess.run([psql_bin, "-X", "-q", "-t", "-A", "-d", conninfo, "-c", sql], capture_output=True, text=True, check=True)
print(res.stdout.strip().splitlines()[-1])
PY
)"

BATCH_STAMP="$(date -u '+%Y%m%dT%H%M%SZ')"
BATCH_DIR="$RESULTS_DIR/batch_${BATCH_ID}_${BATCH_STAMP}_${GIT_SHA:0:8}"
mkdir -p "$BATCH_DIR"
cp "$STATE_DIR/hardware_profile.json" "$BATCH_DIR/hardware_profile.json"
cp "$STATE_DIR/server_instance_info.json" "$BATCH_DIR/server_instance_info.json"
cp "$STATE_DIR/client_instance_info.json" "$BATCH_DIR/client_instance_info.json"
cp "$PG_CONF_FILE" "$BATCH_DIR/postgresql.animal.conf"

log_info "Created Perffarm Batch ID=$BATCH_ID (artifacts: $BATCH_DIR)"

# -----------------------------------------------------------------------------
# Step 4: Initialize Benchmark Cluster & Run Client-Server Baseline Calibration
# -----------------------------------------------------------------------------
stop_existing_bench_server
rm -rf "$PGDATA_DIR" "$PGWAL_DIR"
mkdir -p "$(dirname "$PGDATA_DIR")" "$(dirname "$PGWAL_DIR")"

log_info "Initializing fresh benchmark PostgreSQL cluster (data=$PGDATA_DIR, wal=$PGWAL_DIR)..."
if [[ "$PGWAL_DIR" != "$PGDATA_DIR/pg_wal" ]]; then
    run_on_server "'$PG_INITDB' -D '$PGDATA_DIR' --waldir='$PGWAL_DIR' --encoding=UTF8 --locale=C --no-instructions >/dev/null"
else
    run_on_server "'$PG_INITDB' -D '$PGDATA_DIR' --encoding=UTF8 --locale=C --no-instructions >/dev/null"
fi

# Allow network connections from client host when running across VMs
run_on_server "cat >> '$PGDATA_DIR/pg_hba.conf' <<'EOF'
host    all             all             0.0.0.0/0               trust
host    all             all             ::/0                    trust
EOF"

apply_server_config_and_restart() {
    local test_log="$1"
    local guc_overrides_json="$2"

    stop_existing_bench_server
    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        # Copy base animal conf to remote PGDATA_DIR
        # shellcheck disable=SC2086
        scp -q $SERVER_SSH_OPTS "$PG_CONF_FILE" "$SERVER_SSH_TARGET:$PGDATA_DIR/postgresql.perffarm.conf"
    else
        cp "$PG_CONF_FILE" "$PGDATA_DIR/postgresql.perffarm.conf"
    fi

    python3 - "$PGDATA_DIR/postgresql.perffarm.conf" "$PGPORT" "$guc_overrides_json" > "$WORKDIR/tmp/per_test_override.conf" <<'PY'
import json
import sys

_, port, overrides_raw = sys.argv[1:4]
overrides = json.loads(overrides_raw) if overrides_raw else {}
print(f"include = 'postgresql.perffarm.conf'")
print(f"port = {port}")
for k, v in overrides.items():
    print(f"{k} = {v}")
PY

    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        # shellcheck disable=SC2086
        scp -q $SERVER_SSH_OPTS "$WORKDIR/tmp/per_test_override.conf" "$SERVER_SSH_TARGET:$PGDATA_DIR/postgresql.conf"
        run_on_server "'$PG_CTL' -D '$PGDATA_DIR' -l '$SERVER_WORKDIR/postgres_current.log' -w start >/dev/null"
    else
        cp "$WORKDIR/tmp/per_test_override.conf" "$PGDATA_DIR/postgresql.conf"
        "$PG_CTL" -D "$PGDATA_DIR" -l "$test_log" -w start >/dev/null
    fi

    # Verify server is running and accepting SQL connections
    "$PG_ISREADY" -h "$SERVER_HOST" -p "$PGPORT" -d postgres -q
    "$PG_PSQL" -X -q -t -A -h "$SERVER_HOST" -p "$PGPORT" -d postgres -c "SELECT 1;" >/dev/null
}

CALIBRATION_JSON="$BATCH_DIR/baseline_calibration.json"
if [[ "${RUN_BASELINE_CALIBRATION:-true}" == "true" ]]; then
    log_info "Running client-to-server baseline latency (SELECT 1) & 1 MB throughput (repeat('0123456789', 100000)) calibration..."
    apply_server_config_and_restart "$BATCH_DIR/calibration_postgres.log" "{}"
    python3 "$SCRIPT_DIR/calibrate_baseline.py" \
        --pgbench-bin="$PGBENCH_BIN" \
        --psql-bin="$PG_PSQL" \
        --host="$SERVER_HOST" \
        --port="$PGPORT" \
        --dbname="postgres" \
        --select1-iters="${CALIBRATION_SELECT1_ITERS:-200}" \
        --repeat1mb-iters="${CALIBRATION_REPEAT1MB_ITERS:-50}" \
        ${CLIENT_SSH_TARGET:+--client-ssh-target="$CLIENT_SSH_TARGET"} \
        ${CLIENT_SSH_OPTS:+--client-ssh-opts="$CLIENT_SSH_OPTS"} \
        --output="$CALIBRATION_JSON"

    # Update perffarm_batches with baseline_calibration
    python3 - "$METRICS_PSQL" "$METRICS_DB_CONNINFO" "$METRICS_DB_SCHEMA" "$BATCH_ID" "$CALIBRATION_JSON" <<'PY'
import json
import subprocess
import sys

psql_bin, conninfo, schema, batch_id, cal_path = sys.argv[1:6]
cal = json.loads(open(cal_path, encoding="utf-8").read())
jb = "'" + json.dumps(cal).replace("'", "''") + "'::jsonb"
sql = f'UPDATE "{schema}".perffarm_batches SET baseline_calibration = {jb} WHERE batch_id = {int(batch_id)};'
subprocess.run([psql_bin, "-X", "-q", "-d", conninfo, "-c", sql], check=True)
PY
    S1_MS="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["select_1"]["latency_avg_ms"])' "$CALIBRATION_JSON")"
    S1_QPS="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["select_1"]["qps"])' "$CALIBRATION_JSON")"
    R1_MBS="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["select_repeat_1mb"]["throughput_mb_per_sec"])' "$CALIBRATION_JSON")"
    log_info "Baseline calibration: SELECT 1 avg=${S1_MS} ms (${S1_QPS} QPS) | repeat(100k*'0123456789')=${R1_MBS} MB/s"
    stop_existing_bench_server
else
    echo "{}" > "$CALIBRATION_JSON"
fi

# -----------------------------------------------------------------------------
# Step 5: Per-Test Execution Loop
# -----------------------------------------------------------------------------
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

for idx in $(seq 0 $((TESTS_PLANNED - 1))); do
    TEST_OBJ="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])[int(sys.argv[2])]))' "$SELECTED_TESTS_JSON" "$idx")"
    TEST_NAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["name"])' "$TEST_OBJ")"
    SUITE_NAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["suite"])' "$TEST_OBJ")"
    BUILTIN_NAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("builtin",""))' "$TEST_OBJ")"
    CONFIG_REL="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("config_path",""))' "$TEST_OBJ")"
    TEMPLATE_GROUP="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("template_group",""))' "$TEST_OBJ")"
    FEATURE_PROBE_SQL="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("feature_probe_sql",""))' "$TEST_OBJ")"
    REQ_EXTS="$(python3 -c 'import json,sys; print(" ".join(json.loads(sys.argv[1]).get("required_extensions",[])))' "$TEST_OBJ")"
    GUC_OVERRIDES_JSON="$(python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1]).get("guc_overrides",{})))' "$TEST_OBJ")"

    SCALE="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["effective_profile"].get("scale",1))' "$TEST_OBJ")"
    CLIENTS="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["effective_profile"].get("clients",4))' "$TEST_OBJ")"
    THREADS="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["effective_profile"].get("threads",2))' "$TEST_OBJ")"
    DURATION_SEC="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["effective_profile"].get("duration_sec",0))' "$TEST_OBJ")"
    TRANSACTIONS="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["effective_profile"].get("transactions",0))' "$TEST_OBJ")"

    TEST_DIR="$BATCH_DIR/$(printf '%02d' $((idx + 1)))_${TEST_NAME}"
    mkdir -p "$TEST_DIR"
    SERVER_LOG="$TEST_DIR/postgres.log"
    REPORT_JSON="$TEST_DIR/report.json"
    PRE_SNAP="$TEST_DIR/pg_stats_pre.json"
    POST_SNAP="$TEST_DIR/pg_stats_post.json"
    DELTA_JSON="$TEST_DIR/pg_stats_delta.json"
    BUNDLE_JSON="$TEST_DIR/test_result_bundle.json"
    RUN_LABEL="${ANIMAL_NAME}:b${BATCH_ID}:${TEST_NAME}:${GIT_SHA:0:8}"

    log_info "------------------------------------------------------------------"
    log_info "[$((idx + 1))/$TESTS_PLANNED] Starting test '$TEST_NAME' (suite=$SUITE_NAME, scale=$SCALE, clients=$CLIENTS, threads=$THREADS)"

    TEST_START_ISO="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

    # 5a. Configure & Start Server, Verify Readiness
    apply_server_config_and_restart "$SERVER_LOG" "$GUC_OVERRIDES_JSON"

    # 5b. Probe Required Extensions & Optional SQL Features
    SKIP_REASON=""
    for ext in $REQ_EXTS; do
        EXT_AVAIL="$("$PG_PSQL" -X -q -t -A -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
            -c "SELECT 1 FROM pg_available_extensions WHERE name = '${ext}';" || true)"
        if [[ "$EXT_AVAIL" != "1" ]]; then
            SKIP_REASON="Required PostgreSQL extension '${ext}' is not installed on target server"
            break
        fi
    done

    if [[ -z "$SKIP_REASON" && -n "$FEATURE_PROBE_SQL" ]]; then
        if ! "$PG_PSQL" -X -q -v ON_ERROR_STOP=1 -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
            -c "$FEATURE_PROBE_SQL" >/dev/null 2>"$TEST_DIR/feature_probe.err"; then
            SKIP_REASON="Optional SQL feature unsupported on this branch: $(head -n 1 "$TEST_DIR/feature_probe.err")"
        fi
    fi

    if [[ -n "$SKIP_REASON" ]]; then
        log_info "[$TEST_NAME] SKIPPED: $SKIP_REASON"
        stop_existing_bench_server
        python3 "$SCRIPT_DIR/verify_test_run.py" \
            --batch-id="$BATCH_ID" \
            --test-name="$TEST_NAME" \
            --suite-name="$SUITE_NAME" \
            --run-label="$RUN_LABEL" \
            --status-override="SKIPPED" \
            --skip-reason="$SKIP_REASON" \
            --scale="$SCALE" \
            --clients="$CLIENTS" \
            --threads="$THREADS" \
            --started-at="$TEST_START_ISO" \
            --ended-at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
            --baseline-calibration="$CALIBRATION_JSON" \
            --server-info="$STATE_DIR/server_instance_info.json" \
            --client-info="$STATE_DIR/client_instance_info.json" \
            --bundle-output="$BUNDLE_JSON" \
            --psql-bin="$METRICS_PSQL" \
            --metrics-db-conninfo="$METRICS_DB_CONNINFO" \
            --metrics-db-schema="$METRICS_DB_SCHEMA" >/dev/null
        TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
        continue
    fi

    # 5c. Initialize Benchmark Database (or Clone Cached Template DB)
    BENCH_DB="perffarm_bench"
    "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
        -c "DROP DATABASE IF EXISTS \"${BENCH_DB}\" WITH (FORCE);" >/dev/null 2>&1 || \
        "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
        -c "DROP DATABASE IF EXISTS \"${BENCH_DB}\";" >/dev/null

    INIT_START_NS="$(date +%s%N)"
    TEMPLATE_REUSED=false
    INIT_OK=true

    if [[ "${REUSE_TEMPLATE_DB:-true}" == "true" && -n "$TEMPLATE_GROUP" ]]; then
        TPL_DB="tpl_${TEMPLATE_GROUP}_s${SCALE}"
        TPL_EXISTS="$("$PG_PSQL" -X -q -t -A -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
            -c "SELECT 1 FROM pg_database WHERE datname = '${TPL_DB}';" || true)"
        if [[ "$TPL_EXISTS" != "1" ]]; then
            log_info "[$TEST_NAME] Initializing reusable template DB '$TPL_DB' (scale=$SCALE)..."
            "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres -c "CREATE DATABASE \"${TPL_DB}\";"
            (
                cd "$REPO_ROOT/src/bin/pgbench"
                "$PGBENCH_BIN" --config="$REPO_ROOT/$CONFIG_REL" -i -s "$SCALE" \
                    -h "$SERVER_HOST" -p "$PGPORT" "$TPL_DB" >"$TEST_DIR/init.log" 2>&1
            ) || INIT_OK=false
            if [[ "$INIT_OK" == "true" ]]; then
                "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d "$TPL_DB" \
                    -c "VACUUM ANALYZE;" -c "CHECKPOINT;" >/dev/null
            fi
        else
            log_info "[$TEST_NAME] Cloning benchmark DB from cached template '$TPL_DB'..."
            TEMPLATE_REUSED=true
        fi
        if [[ "$INIT_OK" == "true" ]]; then
            "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
                -c "CREATE DATABASE \"${BENCH_DB}\" TEMPLATE \"${TPL_DB}\";" >/dev/null
        fi
    else
        "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres -c "CREATE DATABASE \"${BENCH_DB}\";"
        if [[ -n "$BUILTIN_NAME" ]]; then
            "$PGBENCH_BIN" -i -q -s "$SCALE" -h "$SERVER_HOST" -p "$PGPORT" "$BENCH_DB" >"$TEST_DIR/init.log" 2>&1 || INIT_OK=false
        else
            (
                cd "$REPO_ROOT/src/bin/pgbench"
                "$PGBENCH_BIN" --config="$REPO_ROOT/$CONFIG_REL" -i -s "$SCALE" \
                    -h "$SERVER_HOST" -p "$PGPORT" "$BENCH_DB" >"$TEST_DIR/init.log" 2>&1
            ) || INIT_OK=false
        fi
    fi

    INIT_END_NS="$(date +%s%N)"
    INIT_DURATION_SEC="$(python3 -c "print(round(($INIT_END_NS - $INIT_START_NS) / 1e9, 4))")"

    if [[ "$INIT_OK" != "true" ]]; then
        log_info "[$TEST_NAME] ERROR: Database initialization failed (see $TEST_DIR/init.log)"
        stop_existing_bench_server
        python3 "$SCRIPT_DIR/verify_test_run.py" \
            --batch-id="$BATCH_ID" \
            --test-name="$TEST_NAME" \
            --suite-name="$SUITE_NAME" \
            --run-label="$RUN_LABEL" \
            --status-override="FAILED" \
            --skip-reason="Database initialization failed" \
            --exit-code=1 \
            --scale="$SCALE" \
            --clients="$CLIENTS" \
            --threads="$THREADS" \
            --init-duration-sec="$INIT_DURATION_SEC" \
            --started-at="$TEST_START_ISO" \
            --ended-at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
            --baseline-calibration="$CALIBRATION_JSON" \
            --server-info="$STATE_DIR/server_instance_info.json" \
            --client-info="$STATE_DIR/client_instance_info.json" \
            --server-log="$SERVER_LOG" \
            --bundle-output="$BUNDLE_JSON" \
            --psql-bin="$METRICS_PSQL" \
            --metrics-db-conninfo="$METRICS_DB_CONNINFO" \
            --metrics-db-schema="$METRICS_DB_SCHEMA" || true
        TESTS_FAILED=$((TESTS_FAILED + 1))
        continue
    fi

    # 5d. Prepare Benchmark Database: VACUUM ANALYZE, CHECKPOINT & Reset Stats
    "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d "$BENCH_DB" \
        -c "VACUUM ANALYZE;" \
        -c "CHECKPOINT;" \
        -c "SELECT pg_stat_reset(); SELECT pg_stat_reset_shared('bgwriter'); SELECT pg_stat_reset_shared('wal');" >/dev/null 2>&1 || true
    if [[ "$PG_MAJOR" -ge 16 ]]; then
        "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d "$BENCH_DB" -c "SELECT pg_stat_reset_shared('io');" >/dev/null 2>&1 || true
    fi
    if [[ "$PG_MAJOR" -ge 17 ]]; then
        "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d "$BENCH_DB" -c "SELECT pg_stat_reset_shared('checkpointer');" >/dev/null 2>&1 || true
    fi
    HAS_PGSS_LIB="$("$PG_PSQL" -X -q -t -A -h "$SERVER_HOST" -p "$PGPORT" -d "$BENCH_DB" -c "SELECT count(*) FROM pg_settings WHERE name = 'shared_preload_libraries' AND setting LIKE '%pg_stat_statements%';" || echo "0")"
    if [[ "${HAS_PGSS_LIB:-0}" -gt 0 ]]; then
        "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d "$BENCH_DB" \
            -c "CREATE EXTENSION IF NOT EXISTS pg_stat_statements; SELECT pg_stat_statements_reset();" >/dev/null 2>&1 || true
    fi

    # 5e. Capture Pre-Benchmark pg_stat_* & OS Snapshot
    python3 "$SCRIPT_DIR/collect_pg_stats.py" snapshot \
        --phase=pre \
        --psql-bin="$PG_PSQL" \
        --host="$SERVER_HOST" \
        --port="$PGPORT" \
        --dbname="$BENCH_DB" \
        ${SERVER_SSH_TARGET:+--server-ssh-target="$SERVER_SSH_TARGET"} \
        ${CLIENT_SSH_TARGET:+--client-ssh-target="$CLIENT_SSH_TARGET"} \
        --output="$PRE_SNAP"

    # 5f. Build & Execute pgbench Benchmark Command
    PGBENCH_ARGS=(
        -h "$SERVER_HOST"
        -p "$PGPORT"
        -s "$SCALE"
        -D "scale=$SCALE"
        -c "$CLIENTS"
        -j "$THREADS"
        --max-tries=3
        -r
        --log-db="$METRICS_DB_CONNINFO"
        --log-db-schema="$METRICS_DB_SCHEMA"
        --log-db-mode="${LOG_DB_MODE:-stream}"
        --log-db-unlogged="${LOG_DB_UNLOGGED:-stream}"
        --run-label="$RUN_LABEL"
        --json-report="$REPORT_JSON"
    )

    if [[ "$DURATION_SEC" -gt 0 ]]; then
        PGBENCH_ARGS+=(-T "$DURATION_SEC")
    elif [[ "$TRANSACTIONS" -gt 0 ]]; then
        PGBENCH_ARGS+=(-t "$TRANSACTIONS")
    fi

    case "${METRICS_LEVEL:-timeseries}" in
        summary)
            ;;
        timeseries)
            PGBENCH_ARGS+=(-P 1 -l --aggregate-interval=1)
            ;;
        full-trace)
            PGBENCH_ARGS+=(-P 1 -l --log-db-index)
            ;;
    esac

    if [[ -n "$BUILTIN_NAME" ]]; then
        PGBENCH_ARGS+=(-b "$BUILTIN_NAME")
    else
        PGBENCH_ARGS=(--config="$REPO_ROOT/$CONFIG_REL" "${PGBENCH_ARGS[@]}")
    fi
    PGBENCH_ARGS+=("$BENCH_DB")

    log_info "[$TEST_NAME] Running benchmark (metrics_level=$METRICS_LEVEL)..."
    set +e
    (
        cd "$REPO_ROOT/src/bin/pgbench"
        "$PGBENCH_BIN" "${PGBENCH_ARGS[@]}" >"$TEST_DIR/pgbench.out" 2>"$TEST_DIR/pgbench.err"
    )
    PGBENCH_EXIT=$?
    set -e

    # 5g. Capture Post-Benchmark pg_stat_* & OS Snapshot and Compute Delta
    python3 "$SCRIPT_DIR/collect_pg_stats.py" snapshot \
        --phase=post \
        --psql-bin="$PG_PSQL" \
        --host="$SERVER_HOST" \
        --port="$PGPORT" \
        --dbname="$BENCH_DB" \
        ${SERVER_SSH_TARGET:+--server-ssh-target="$SERVER_SSH_TARGET"} \
        ${CLIENT_SSH_TARGET:+--client-ssh-target="$CLIENT_SSH_TARGET"} \
        --output="$POST_SNAP"

    python3 "$SCRIPT_DIR/collect_pg_stats.py" delta \
        --pre="$PRE_SNAP" \
        --post="$POST_SNAP" \
        --output="$DELTA_JSON"

    # Drop working bench_db (preserving cached template DBs if any) and stop server cleanly
    "$PG_PSQL" -X -q -h "$SERVER_HOST" -p "$PGPORT" -d postgres \
        -c "DROP DATABASE IF EXISTS \"${BENCH_DB}\";" >/dev/null 2>&1 || true
    stop_existing_bench_server

    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        # shellcheck disable=SC2086
        scp -q $SERVER_SSH_OPTS "$SERVER_SSH_TARGET:$SERVER_WORKDIR/postgres_current.log" "$SERVER_LOG" || true
    fi

    # 5h. Verify Test Run (5-Point Check) & Record in perffarm_test_runs
    TPL_FLAG=""
    [[ "$TEMPLATE_REUSED" == "true" ]] && TPL_FLAG="--template-reused"

    set +e
    VERIFY_OUT="$(python3 "$SCRIPT_DIR/verify_test_run.py" \
        --batch-id="$BATCH_ID" \
        --test-name="$TEST_NAME" \
        --suite-name="$SUITE_NAME" \
        --run-label="$RUN_LABEL" \
        --exit-code="$PGBENCH_EXIT" \
        --scale="$SCALE" \
        --clients="$CLIENTS" \
        --threads="$THREADS" \
        --init-duration-sec="$INIT_DURATION_SEC" \
        ${TPL_FLAG:+$TPL_FLAG} \
        --started-at="$TEST_START_ISO" \
        --ended-at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
        --json-report="$REPORT_JSON" \
        --pg-stats-delta="$DELTA_JSON" \
        --baseline-calibration="$CALIBRATION_JSON" \
        --server-info="$STATE_DIR/server_instance_info.json" \
        --client-info="$STATE_DIR/client_instance_info.json" \
        --server-log="$SERVER_LOG" \
        --bundle-output="$BUNDLE_JSON" \
        --psql-bin="$METRICS_PSQL" \
        --metrics-db-conninfo="$METRICS_DB_CONNINFO" \
        --metrics-db-schema="$METRICS_DB_SCHEMA")"
    VERIFY_RC=$?
    set -e

    log_info "[$TEST_NAME] Verification result: $VERIFY_OUT"
    if [[ $VERIFY_RC -eq 0 ]]; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi

    # 5i. Upload Per-Test Result
    bash "$SCRIPT_DIR/upload_results.sh" \
        --config="$CONF_FILE" \
        --test-dir="$TEST_DIR" \
        --psql-bin="$METRICS_PSQL"
done

# -----------------------------------------------------------------------------
# Step 6: Finalize Batch, Package Archive, Upload & Optional Cloud Teardown
# -----------------------------------------------------------------------------
FINAL_BATCH_STATUS="completed"
if [[ $TESTS_FAILED -gt 0 ]]; then
    FINAL_BATCH_STATUS="completed_with_failures"
fi

"$METRICS_PSQL" -X -q -d "$METRICS_DB_CONNINFO" -c "
    UPDATE \"${METRICS_DB_SCHEMA}\".perffarm_batches
    SET ended_at = clock_timestamp(),
        status = '${FINAL_BATCH_STATUS}',
        tests_passed = ${TESTS_PASSED},
        tests_failed = ${TESTS_FAILED},
        tests_skipped = ${TESTS_SKIPPED}
    WHERE batch_id = ${BATCH_ID};
"

# Write batch_manifest.json
python3 - "$BATCH_DIR/batch_manifest.json" "$BATCH_ID" "$ANIMAL_NAME" "$TOPOLOGY" \
    "$FIRST_BRANCH" "$GIT_SHA" "$GIT_TS" "$GIT_SUBJECT" "$TEST_PROFILE" "$METRICS_LEVEL" \
    "$TESTS_PLANNED" "$TESTS_PASSED" "$TESTS_FAILED" "$TESTS_SKIPPED" "$FINAL_BATCH_STATUS" \
    "$CALIBRATION_JSON" "$STATE_DIR/server_instance_info.json" "$STATE_DIR/client_instance_info.json" <<'PY'
import json
import os
import sys
from datetime import datetime, timezone

(
    out_path,
    batch_id,
    animal,
    topology,
    branch,
    sha,
    commit_ts,
    subject,
    profile,
    mlevel,
    planned,
    passed,
    failed,
    skipped,
    status,
    cal_path,
    srv_path,
    cli_path,
) = sys.argv[1:19]

manifest = {
    "batch_id": int(batch_id),
    "animal_name": animal,
    "topology": topology,
    "pg_branch": branch,
    "git_commit_sha": sha,
    "git_commit_ts": commit_ts,
    "git_commit_subject": subject,
    "test_profile": profile,
    "metrics_level": mlevel,
    "tests_planned": int(planned),
    "tests_passed": int(passed),
    "tests_failed": int(failed),
    "tests_skipped": int(skipped),
    "status": status,
    "completed_at_utc": datetime.now(timezone.utc).isoformat(),
    "baseline_calibration": json.loads(open(cal_path).read()) if os.path.exists(cal_path) else {},
    "server_instance_info": json.loads(open(srv_path).read()) if os.path.exists(srv_path) else {},
    "client_instance_info": json.loads(open(cli_path).read()) if os.path.exists(cli_path) else {},
}
open(out_path, "w", encoding="utf-8").write(json.dumps(manifest, indent=2) + "\n")
PY

bash "$SCRIPT_DIR/upload_results.sh" \
    --config="$CONF_FILE" \
    --batch-dir="$BATCH_DIR" \
    --batch-id="$BATCH_ID" \
    --psql-bin="$METRICS_PSQL"

if [[ "${AUTO_TEARDOWN:-false}" == "true" ]]; then
    bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" teardown
fi

log_info "=================================================================="
log_info "Perffarm Batch $BATCH_ID Complete! Status: $FINAL_BATCH_STATUS"
log_info "  Passed: $TESTS_PASSED | Failed: $TESTS_FAILED | Skipped: $TESTS_SKIPPED"
log_info "  Batch Artifacts: $BATCH_DIR"
log_info "=================================================================="
exit 0
