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
COMMIT_OVERRIDE=""
COMMITS_LIST_OVERRIDE=""
COMMITS_FILE_OVERRIDE=""
CI_STATUS_OVERRIDE=""
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
SKIP_SETUP=false
ORIG_ARGS=("$@")

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
        --commit=*)
            COMMIT_OVERRIDE="${1#*=}"
            shift
            ;;
        --commit)
            COMMIT_OVERRIDE="$2"
            shift 2
            ;;
        --commits=*)
            COMMITS_LIST_OVERRIDE="${1#*=}"
            shift
            ;;
        --commits)
            COMMITS_LIST_OVERRIDE="$2"
            shift 2
            ;;
        --commits-file=*)
            COMMITS_FILE_OVERRIDE="${1#*=}"
            shift
            ;;
        --commits-file)
            COMMITS_FILE_OVERRIDE="$2"
            shift 2
            ;;
        --ci-status=*)
            CI_STATUS_OVERRIDE="${1#*=}"
            shift
            ;;
        --ci-status)
            CI_STATUS_OVERRIDE="$2"
            shift 2
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
        --skip-setup)
            SKIP_SETUP=true
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --config=PATH               Path to animal.conf (default: perffarm/conf/animal.conf)
  --branch=NAME               PostgreSQL git branch to test (overrides PG_BRANCHES)
  --commit=SHA                Specific PostgreSQL commit SHA/ref to check out and benchmark
  --commits=SHA1,SHA2,...     Comma- or space-separated list of commits to benchmark sequentially
  --commits-file=PATH         File containing commit SHAs (one per line) to benchmark sequentially
  --ci-status=STATUS          Explicit CI status (e.g., github_success) when pre-verified
  --profile=TIER              Sizing profile: smoke | standard | nightly (default: from animal.conf)
  --tests=LIST                Comma-separated test or suite names, or 'all'
  --metrics-level=LEVEL       Telemetry granularity: summary | timeseries | full-trace
  --upload-level=LEVEL        Upload granularity: summary | timeseries | full-trace
  --topology=MODE             local | remote-server | remote-client | both-remote
  --server-ssh=USER@HOST      Remote PostgreSQL server SSH target
  --client-ssh=USER@HOST      Remote pgbench client SSH target
  --prebuilt-pg-config=PATH   Use an existing pg_config binary instead of building PostgreSQL from git
  --local-pg-src=DIR          Use an existing local PostgreSQL git checkout (or reference clone)
  --verify-ci=MODE            auto | strict | off
  --skip-ci-check             Alias for --verify-ci=off
  --check-world=MODE          full | fast | skip
  --fast-check                Alias for --check-world=fast
  --skip-check-world          Alias for --check-world=skip
  --force                     Run even if this commit was already tested for this animal & profile
  --no-template-reuse         Re-initialize database from scratch for every test
  --skip-calibration          Skip client-server SELECT 1 / repeat('0123456789', 100000) calibration
  --skip-setup                Skip Step 0 machine discovery & Metrics DB setup (for multi-commit loops)
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
if [[ "$SKIP_SETUP" != "true" ]]; then
    if [[ "${AUTO_PROVISION:-false}" == "true" ]]; then
        bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" provision
    fi

    bash "$SCRIPT_DIR/setup_machine.sh" --config="$CONF_FILE"
    if [[ "$TOPOLOGY" != "local" ]]; then
        bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" sync-repo
    fi

    PG_CONFIG_FOR_METRICS="${METRICS_PG_CONFIG:-${PREBUILT_PG_CONFIG:-${PG_CONFIG:-}}}"
    bash "$SCRIPT_DIR/setup_metrics_db.sh" --config="$CONF_FILE" \
        ${PG_CONFIG_FOR_METRICS:+--pg-config="$PG_CONFIG_FOR_METRICS"} --ensure
fi

# -----------------------------------------------------------------------------
# Step 1: Checkout Source Tree & Verify CI Status
# -----------------------------------------------------------------------------
# Handle multi-commit sequential execution (--commits or --commits-file)
if [[ -z "$COMMIT_OVERRIDE" && ( -n "$COMMITS_LIST_OVERRIDE" || -n "$COMMITS_FILE_OVERRIDE" ) ]]; then
    COMMIT_LIST=()
    if [[ -n "$COMMITS_LIST_OVERRIDE" ]]; then
        read -r -a EXTRA_COMMITS <<< "$(echo "$COMMITS_LIST_OVERRIDE" | tr ',' ' ')"
        COMMIT_LIST+=("${EXTRA_COMMITS[@]}")
    fi
    if [[ -n "$COMMITS_FILE_OVERRIDE" ]]; then
        if [[ ! -f "$COMMITS_FILE_OVERRIDE" ]]; then
            echo "[run_perffarm] ERROR: Commits file not found: $COMMITS_FILE_OVERRIDE" >&2
            exit 1
        fi
        while IFS= read -r line || [[ -n "$line" ]]; do
            # Strip comments and leading/trailing whitespace, take first token as SHA
            clean_line="${line%%#*}"
            sha_tok="$(echo "$clean_line" | awk '{print $1}')"
            if [[ -n "$sha_tok" ]]; then
                COMMIT_LIST+=("$sha_tok")
            fi
        done < "$COMMITS_FILE_OVERRIDE"
    fi

    log_info "Multi-commit run configured (${#COMMIT_LIST[@]} commits); executing sequentially..."
    MULTI_COMMIT_RC=0
    COMMIT_IDX=0
    COMMITS_SUCCEEDED=0
    COMMITS_FAILED=0
    FAILED_SHAS=()
    for sha in "${COMMIT_LIST[@]}"; do
        COMMIT_IDX=$((COMMIT_IDX + 1))
        log_info "=== Multi-commit progress: [${COMMIT_IDX}/${#COMMIT_LIST[@]}] commit ${sha:0:12} ==="
        if AUTO_PROVISION=false AUTO_TEARDOWN=false bash "$0" "${ORIG_ARGS[@]}" --commit="$sha" --skip-setup; then
            COMMITS_SUCCEEDED=$((COMMITS_SUCCEEDED + 1))
        else
            rc=$?
            MULTI_COMMIT_RC="$rc"
            COMMITS_FAILED=$((COMMITS_FAILED + 1))
            FAILED_SHAS+=("${sha:0:12}")
            log_info "WARNING: Commit ${sha:0:12} failed with exit code $rc"
        fi
    done
    if [[ "${AUTO_TEARDOWN:-false}" == "true" ]]; then
        bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" teardown
    fi
    log_info "=================================================================="
    log_info "Multi-commit sweep complete: ${COMMITS_SUCCEEDED}/${#COMMIT_LIST[@]} succeeded, ${COMMITS_FAILED} failed."
    if [[ ${#FAILED_SHAS[@]} -gt 0 ]]; then
        log_info "Failed commits: ${FAILED_SHAS[*]}"
    fi
    log_info "=================================================================="
    exit "$MULTI_COMMIT_RC"
fi

read -r -a BRANCH_LIST <<< "$(echo "$PG_BRANCHES" | tr ',' ' ')"
if [[ ${#BRANCH_LIST[@]} -gt 1 && -z "$COMMIT_OVERRIDE" ]]; then
    log_info "Multiple branches configured (${BRANCH_LIST[*]}); executing Perffarm batch per branch..."
    MULTI_RC=0
    for br in "${BRANCH_LIST[@]}"; do
        AUTO_PROVISION=false AUTO_TEARDOWN=false bash "$0" "${ORIG_ARGS[@]}" --branch="$br" --skip-setup || MULTI_RC=$?
    done
    if [[ "${AUTO_TEARDOWN:-false}" == "true" ]]; then
        bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" teardown
    fi
    exit "$MULTI_RC"
fi

FIRST_BRANCH="${BRANCH_LIST[0]:-master}"
PG_SRC_DIR="$WORKDIR/src/postgres"

GIT_SHA=""
GIT_TS=""
GIT_SUBJECT=""
CI_STATUS="not_checked"
HAS_GIT_CHECKOUT=false

if [[ -n "${PG_LOCAL_REPO:-}" && -d "${PG_LOCAL_REPO}/.git" && -z "$COMMIT_OVERRIDE" ]]; then
    PG_SRC_DIR="$PG_LOCAL_REPO"
    HAS_GIT_CHECKOUT=true
    GIT_SHA="$(git -C "$PG_SRC_DIR" rev-parse HEAD)"
    GIT_TS="$(git -C "$PG_SRC_DIR" show -s --format=%cI HEAD)"
    GIT_SUBJECT="$(git -C "$PG_SRC_DIR" show -s --format=%s HEAD)"
    LOCAL_BR="$(git -C "$PG_SRC_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "$FIRST_BRANCH")"
    if [[ "$LOCAL_BR" != "HEAD" && -z "$BRANCH_OVERRIDE" ]]; then
        FIRST_BRANCH="$LOCAL_BR"
    fi
    log_info "Using local PostgreSQL git tree at $PG_SRC_DIR ($FIRST_BRANCH @ ${GIT_SHA:0:12})"
elif [[ -n "${PREBUILT_PG_CONFIG:-}" ]] && command -v "${PREBUILT_PG_CONFIG}" >/dev/null 2>&1 && [[ -z "$COMMIT_OVERRIDE" ]]; then
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
    mkdir -p "$(dirname "$PG_SRC_DIR")"
    if [[ ! -d "$PG_SRC_DIR/.git" ]]; then
        if [[ -n "${PG_LOCAL_REPO:-}" && -d "${PG_LOCAL_REPO}/.git" ]]; then
            log_info "Cloning PostgreSQL worktree into $PG_SRC_DIR using local reference $PG_LOCAL_REPO..."
            git clone --shared "$PG_LOCAL_REPO" "$PG_SRC_DIR"
            git -C "$PG_SRC_DIR" remote set-url origin "$PG_GIT_URL" || true
        else
            log_info "Cloning PostgreSQL branch '$FIRST_BRANCH' from $PG_GIT_URL..."
            git clone --branch "$FIRST_BRANCH" "$PG_GIT_URL" "$PG_SRC_DIR"
        fi
    fi

    if [[ -n "$COMMIT_OVERRIDE" ]]; then
        log_info "Checking out requested commit '$COMMIT_OVERRIDE' on branch '$FIRST_BRANCH'..."
        if ! git -C "$PG_SRC_DIR" cat-file -e "${COMMIT_OVERRIDE}^{commit}" 2>/dev/null; then
            git -C "$PG_SRC_DIR" fetch origin "$FIRST_BRANCH:refs/remotes/origin/$FIRST_BRANCH" >/dev/null 2>&1 || \
                git -C "$PG_SRC_DIR" fetch origin "$FIRST_BRANCH" >/dev/null 2>&1 || \
                git -C "$PG_SRC_DIR" fetch origin "$COMMIT_OVERRIDE" >/dev/null 2>&1 || true
        fi
        if ! git -C "$PG_SRC_DIR" checkout -f "$COMMIT_OVERRIDE" >/dev/null 2>&1; then
            echo "[run_perffarm] ERROR: Failed to check out commit '$COMMIT_OVERRIDE' in $PG_SRC_DIR" >&2
            exit 1
        fi
    else
        log_info "Checking out latest PostgreSQL branch '$FIRST_BRANCH' from $PG_GIT_URL..."
        git -C "$PG_SRC_DIR" fetch origin "$FIRST_BRANCH:refs/remotes/origin/$FIRST_BRANCH" >/dev/null 2>&1 || \
            git -C "$PG_SRC_DIR" fetch origin "$FIRST_BRANCH" >/dev/null 2>&1
        if ! (git -C "$PG_SRC_DIR" checkout -f "origin/$FIRST_BRANCH" >/dev/null 2>&1 || \
              git -C "$PG_SRC_DIR" checkout -f FETCH_HEAD >/dev/null 2>&1); then
            echo "[run_perffarm] ERROR: Failed to check out branch '$FIRST_BRANCH' in $PG_SRC_DIR" >&2
            exit 1
        fi
    fi
    HAS_GIT_CHECKOUT=true
    GIT_SHA="$(git -C "$PG_SRC_DIR" rev-parse HEAD)"
    GIT_TS="$(git -C "$PG_SRC_DIR" show -s --format=%cI HEAD)"
    GIT_SUBJECT="$(git -C "$PG_SRC_DIR" show -s --format=%s HEAD)"
fi

# Verify Upstream CI Status (auto | strict | off) with CI_FALLBACK_COMMITS walkback
if [[ -n "$CI_STATUS_OVERRIDE" ]]; then
    CI_STATUS="$CI_STATUS_OVERRIDE"
    log_info "Using explicit CI status override: $CI_STATUS"
elif [[ "$VERIFY_CI" == "off" ]]; then
    CI_STATUS="skipped"
else
    FALLBACK_MAX="${CI_FALLBACK_COMMITS:-10}"
    if [[ -n "$COMMIT_OVERRIDE" ]]; then
        FALLBACK_MAX=0
    fi
    log_info "Verifying CI status for commit ${GIT_SHA:0:12} (mode=$VERIFY_CI, fallback_commits=$FALLBACK_MAX)..."
    CI_CHECK_OUT="$(python3 - "$GIT_SHA" "$VERIFY_CI" "$FALLBACK_MAX" "$PG_SRC_DIR" "$HAS_GIT_CHECKOUT" <<'PY'
import json
import os
import subprocess
import sys
import urllib.request

head_sha, mode, fallback_str, pg_src_dir, has_git_str = sys.argv[1:6]
max_fallback = max(0, int(fallback_str or 0))
has_git = has_git_str == "true"

gh_token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
if not gh_token:
    try:
        tok_res = subprocess.run(
            ["gh", "auth", "token"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
        if tok_res.returncode == 0 and tok_res.stdout.strip():
            gh_token = tok_res.stdout.strip()
    except Exception:
        pass

def check_commit_ci(sha: str) -> str:
    headers = {
        "User-Agent": "pgbench4perffarm/1.0",
        "Accept": "application/vnd.github+json",
    }
    if gh_token:
        headers["Authorization"] = f"Bearer {gh_token}"

    checks_state = "unknown"
    any_api_ok = False
    has_check_runs = False
    try:
        req2 = urllib.request.Request(
            f"https://api.github.com/repos/postgres/postgres/commits/{sha}/check-runs",
            headers=headers,
        )
        with urllib.request.urlopen(req2, timeout=5) as resp2:
            any_api_ok = True
            cdata = json.loads(resp2.read().decode("utf-8"))
            runs = cdata.get("check_runs", [])
            if runs:
                has_check_runs = True
                conclusions = [r.get("conclusion") for r in runs if r.get("status") == "completed"]
                if any(c in ("failure", "timed_out", "cancelled", "action_required") for c in conclusions):
                    checks_state = "failure"
                elif len(conclusions) == len(runs) and any(c == "success" for c in conclusions):
                    checks_state = "success"
                else:
                    checks_state = "pending"
    except Exception:
        pass

    status_state = "unknown"
    if not has_check_runs:
        try:
            req = urllib.request.Request(
                f"https://api.github.com/repos/postgres/postgres/commits/{sha}/status",
                headers=headers,
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                any_api_ok = True
                data = json.loads(resp.read().decode("utf-8"))
                if data.get("total_count", 0) > 0:
                    status_state = data.get("state", "unknown")
        except Exception:
            pass

    if not any_api_ok:
        return "local_or_unverified"
    if status_state == "failure" or checks_state == "failure":
        return "github_failure"
    if status_state == "success" or checks_state == "success":
        return "github_success"
    if status_state == "pending" or checks_state == "pending":
        return "github_pending"
    return f"github_{status_state}"

candidates = [head_sha]
if has_git and max_fallback > 0:
    try:
        res = subprocess.run(
            ["git", "-C", pg_src_dir, "rev-list", "-n", str(max_fallback + 1), head_sha],
            capture_output=True,
            text=True,
            check=True,
        )
        revs = [line.strip() for line in res.stdout.splitlines() if line.strip()]
        if revs:
            candidates = revs
    except Exception:
        pass

chosen_sha = head_sha
chosen_status = check_commit_ci(head_sha)
if chosen_status == "github_failure" and len(candidates) > 1:
    for cand in candidates[1:]:
        st = check_commit_ci(cand)
        if st in ("github_success", "github_pending", "github_unknown"):
            chosen_sha = cand
            chosen_status = st
            break

print(f"{chosen_sha} {chosen_status}")
PY
)"
    NEW_SHA="$(echo "$CI_CHECK_OUT" | awk '{print $1}')"
    CI_STATUS="$(echo "$CI_CHECK_OUT" | awk '{print $2}')"
    if [[ -n "$NEW_SHA" && "$NEW_SHA" != "$GIT_SHA" && "$HAS_GIT_CHECKOUT" == "true" ]]; then
        log_info "HEAD commit ${GIT_SHA:0:12} had failing CI; falling back to commit ${NEW_SHA:0:12} ($CI_STATUS)"
        git -C "$PG_SRC_DIR" checkout -f "$NEW_SHA" >/dev/null 2>&1 || true
        GIT_SHA="$NEW_SHA"
        GIT_TS="$(git -C "$PG_SRC_DIR" show -s --format=%cI "$GIT_SHA")"
        GIT_SUBJECT="$(git -C "$PG_SRC_DIR" show -s --format=%s "$GIT_SHA")"
    fi
    if [[ "$VERIFY_CI" == "strict" && "$CI_STATUS" != "github_success" ]]; then
        echo "[run_perffarm] ERROR: Strict CI check failed for $GIT_SHA (status=$CI_STATUS)" >&2
        exit 1
    fi
fi

# Resolve Metrics DB psql binary
resolve_metrics_psql() {
    local candidate bindir
    local candidates=(
        "${METRICS_PG_CONFIG:-}"
        "$WORKDIR/metrics_pg_install/bin/pg_config"
        "${PREBUILT_PG_CONFIG:-}"
        "${PG_CONFIG:-}"
        "pg_config"
        "$HOME/pg20_install/bin/pg_config"
        "$HOME/pg18/bin/pg_config"
        "$WORKDIR/install/bin/pg_config"
    )
    for sys_pgc in /usr/lib/postgresql/*/bin/pg_config; do
        if [[ -x "$sys_pgc" ]]; then
            candidates+=("$sys_pgc")
        fi
    done
    for candidate in "${candidates[@]}"; do
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
# Step 2: Configure & Build PostgreSQL + pgbench4perffarm (+ optional pgvector)
# -----------------------------------------------------------------------------
BUILD_START_TS="$(date +%s)"
EFFECTIVE_CFG_FLAGS="$(cat "$STATE_DIR/effective_configure_flags.txt" 2>/dev/null || echo "${CONFIGURE_FLAGS:-}")"
EFFECTIVE_CC="${CC:-gcc}"
if [[ "${USE_CCACHE:-auto}" == "true" || "${USE_CCACHE:-auto}" == "auto" ]]; then
    if command -v ccache >/dev/null 2>&1 && [[ "$EFFECTIVE_CC" != *ccache* ]]; then
        export CCACHE_DIR="${CCACHE_DIR:-$WORKDIR/ccache}"
        mkdir -p "$CCACHE_DIR"
        EFFECTIVE_CC="ccache $EFFECTIVE_CC"
    fi
fi

record_failed_build_and_exit() {
    local fail_stage="$1"
    local fail_log="$2"
    local b_dur=$(( $(date +%s) - BUILD_START_TS ))
    echo "[run_perffarm] ERROR: Build failed during $fail_stage for commit ${GIT_SHA:0:12} (see $fail_log)" >&2
    if [[ -f "$fail_log" ]]; then
        tail -n 20 "$fail_log" >&2 || true
    fi
    python3 - "$METRICS_PSQL" "$METRICS_DB_CONNINFO" "$METRICS_DB_SCHEMA" \
        "$ANIMAL_NAME" "$TOPOLOGY" "$TEST_PROFILE" "$METRICS_LEVEL" "$FIRST_BRANCH" \
        "$GIT_SHA" "$GIT_TS" "$GIT_SUBJECT" "$CI_STATUS" "$EFFECTIVE_CFG_FLAGS" "${CFLAGS:-}" \
        "$b_dur" "$STATE_DIR/server_instance_info.json" "$STATE_DIR/client_instance_info.json" <<'PY' || true
import json, os, subprocess, sys
(psql_bin, conninfo, schema, animal, topology, profile, mlevel, branch,
 sha, commit_ts, subject, ci_status, cfg_flags, cflags, b_dur, srv_path, cli_path) = sys.argv[1:18]
srv = json.loads(open(srv_path, encoding="utf-8").read()) if os.path.exists(srv_path) else {}
cli = json.loads(open(cli_path, encoding="utf-8").read()) if os.path.exists(cli_path) else {}
def lit(v):
    return "NULL" if (v is None or v == "") else "'" + str(v).replace("'", "''") + "'"
def jb(o):
    return "'" + json.dumps(o).replace("'", "''") + "'::jsonb"
sql = f"""
SET search_path TO "{schema}", public, pg_catalog;
INSERT INTO perffarm_batches (
    animal_name, topology, test_profile, metrics_level,
    pg_branch, pg_version_num, git_commit_sha, git_commit_ts, git_commit_subject,
    ci_status, configure_flags, cflags,
    build_status, build_duration_sec, check_world_status, check_world_duration_sec,
    server_instance_info, client_instance_info, tests_planned, tests_failed, status, ended_at
) VALUES (
    {lit(animal)}, {lit(topology)}, {lit(profile)}, {lit(mlevel)},
    {lit(branch)}, 200000, {lit(sha)}, {lit(commit_ts)}::timestamptz, {lit(subject)},
    {lit(ci_status)}, {lit(cfg_flags)}, {lit(cflags)},
    'failed', {float(b_dur)}, 'skipped', 0,
    {jb(srv)}, {jb(cli)}, 0, 0, 'build_failed', clock_timestamp()
);
"""
subprocess.run([psql_bin, "-X", "-q", "-d", conninfo, "-c", sql], check=False)
PY
    exit 1
}

if [[ -n "${PREBUILT_PG_CONFIG:-}" ]] && command -v "${PREBUILT_PG_CONFIG}" >/dev/null 2>&1 && [[ -z "$COMMIT_OVERRIDE" ]]; then
    TARGET_PG_CONFIG="$(command -v "$PREBUILT_PG_CONFIG")"
    BUILD_STATUS="prebuilt"
else
    log_info "Configuring and building PostgreSQL in $PG_SRC_DIR (CC='$EFFECTIVE_CC', jobs=$BUILD_JOBS)..."
    set +e
    (
        set -e
        cd "$PG_SRC_DIR"
        CFG_INPUT_HASH="$(sha256sum configure configure.ac src/include/pg_config.h.in 2>/dev/null | sha256sum | awk '{print $1}')"
        CFG_SIG="${EFFECTIVE_CC}|${CFLAGS:--O2 -g -fno-omit-frame-pointer}|${EFFECTIVE_CFG_FLAGS}|${CFG_INPUT_HASH}"
        NEED_CONFIGURE=true
        if [[ -x "config.status" && -f ".perffarm_cfg_sig" && "$(cat .perffarm_cfg_sig 2>/dev/null)" == "$CFG_SIG" ]]; then
            NEED_CONFIGURE=false
        fi

        if [[ "$NEED_CONFIGURE" == "true" ]]; then
            # shellcheck disable=SC2086
            ./configure --prefix="$WORKDIR/install" --enable-depend CC="$EFFECTIVE_CC" CFLAGS="${CFLAGS:--O2 -g -fno-omit-frame-pointer}" $EFFECTIVE_CFG_FLAGS >"$LOG_DIR/pg_configure.log" 2>&1
            echo "$CFG_SIG" > .perffarm_cfg_sig
        fi

        if ! make -j"$BUILD_JOBS" >"$LOG_DIR/pg_build.log" 2>&1; then
            log_info "Incremental build encountered an error; running clean configure and build..."
            make distclean >/dev/null 2>&1 || true
            # shellcheck disable=SC2086
            ./configure --prefix="$WORKDIR/install" --enable-depend CC="$EFFECTIVE_CC" CFLAGS="${CFLAGS:--O2 -g -fno-omit-frame-pointer}" $EFFECTIVE_CFG_FLAGS >"$LOG_DIR/pg_configure.log" 2>&1
            echo "$CFG_SIG" > .perffarm_cfg_sig
            make -j"$BUILD_JOBS" >"$LOG_DIR/pg_build.log" 2>&1
        fi

        make install >"$LOG_DIR/pg_install.log" 2>&1
        if [[ -d "contrib/pg_stat_statements" ]]; then
            make -C contrib/pg_stat_statements -j"$BUILD_JOBS" install >>"$LOG_DIR/pg_install.log" 2>&1 || true
        fi
    )
    PG_BUILD_RC=$?
    set -e
    if [[ $PG_BUILD_RC -ne 0 ]]; then
        record_failed_build_and_exit "PostgreSQL build/install" "$LOG_DIR/pg_build.log"
    fi
    TARGET_PG_CONFIG="$WORKDIR/install/bin/pg_config"
    BUILD_STATUS="passed"
fi

log_info "Building and installing pgbench4perffarm (contrib/pgbench + src/bin/pgbench) against $TARGET_PG_CONFIG..."
make -C "$REPO_ROOT" PG_CONFIG="$TARGET_PG_CONFIG" clean >"$LOG_DIR/pgbench4perffarm_build.log" 2>&1 || true
if ! (make -C "$REPO_ROOT" -j"$BUILD_JOBS" PG_CONFIG="$TARGET_PG_CONFIG" CC="$EFFECTIVE_CC" all >>"$LOG_DIR/pgbench4perffarm_build.log" 2>&1 && \
      make -C "$REPO_ROOT" PG_CONFIG="$TARGET_PG_CONFIG" install >>"$LOG_DIR/pgbench4perffarm_build.log" 2>&1); then
    record_failed_build_and_exit "pgbench4perffarm build/install" "$LOG_DIR/pgbench4perffarm_build.log"
fi

if [[ "${BUILD_PGVECTOR:-false}" == "true" ]]; then
    PGVEC_DIR="$WORKDIR/src/pgvector"
    log_info "Building and installing pgvector (${PGVECTOR_BRANCH:-v0.8.0}) against $TARGET_PG_CONFIG..."
    if [[ ! -d "$PGVEC_DIR/.git" ]]; then
        git clone --branch "${PGVECTOR_BRANCH:-v0.8.0}" "${PGVECTOR_GIT_URL:-https://github.com/pgvector/pgvector.git}" "$PGVEC_DIR" >"$LOG_DIR/pgvector_build.log" 2>&1 || true
    fi
    if [[ -d "$PGVEC_DIR" ]]; then
        make -C "$PGVEC_DIR" -j"$BUILD_JOBS" PG_CONFIG="$TARGET_PG_CONFIG" all install >>"$LOG_DIR/pgvector_build.log" 2>&1 || \
            log_info "WARNING: pgvector build/install failed (see $LOG_DIR/pgvector_build.log)"
    fi
fi

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
    run_on_server "if [[ -f '$PGDATA_DIR/postmaster.pid' ]]; then '$PG_CTL' -D '$PGDATA_DIR' -m fast stop; fi" >/dev/null 2>&1 || true
}

CHECK_PGDATA="$WORKDIR/tmp/check_pgdata"
CHECK_PGWAL="$WORKDIR/tmp/check_pgwal"
CHECK_PGPORT=$(( PGPORT + 10 ))

stop_local_check_server() {
    if [[ -f "$CHECK_PGDATA/postmaster.pid" ]]; then
        "$PG_CTL" -D "$CHECK_PGDATA" -m fast stop >/dev/null 2>&1 || true
    fi
    rm -rf "$CHECK_PGDATA" "$CHECK_PGWAL"
}

verify_check_server_log() {
    local clog="$1"
    python3 - "$SCRIPT_DIR" "$clog" <<'PY'
import sys
script_dir, log_path = sys.argv[1:3]
sys.path.insert(0, script_dir)
import verify_test_run as v
res = v.inspect_server_log(log_path)
if not res.get("clean", True):
    for line in res.get("severe_error_lines", []):
        sys.stderr.write(f"[check_server.log] {line}\n")
    sys.exit(1)
PY
}

if [[ "$CHECK_WORLD_MODE" == "full" ]]; then
    log_info "Running full 'make check-world' and pgbench4perffarm verification..."
    CW_OK=true
    if [[ -d "$PG_SRC_DIR" && -f "$PG_SRC_DIR/GNUmakefile" && "$BUILD_STATUS" != "prebuilt" ]]; then
        make -C "$PG_SRC_DIR" -j"${CHECK_WORLD_JOBS:-$BUILD_JOBS}" check-world >"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    fi
    stop_local_check_server
    mkdir -p "$WORKDIR/tmp"
    rm -f "$LOG_DIR/check_server.log"
    "$PG_INITDB" -D "$CHECK_PGDATA" --waldir="$CHECK_PGWAL" --encoding=UTF8 --locale=C --no-instructions >/dev/null
    {
        echo "port = $CHECK_PGPORT"
        echo "unix_socket_directories = '/tmp'"
    } >> "$CHECK_PGDATA/postgresql.conf"
    "$PG_CTL" -D "$CHECK_PGDATA" -l "$LOG_DIR/check_server.log" -w start >/dev/null
    make -C "$REPO_ROOT/contrib/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" PGPORT="$CHECK_PGPORT" PGHOST="127.0.0.1" installcheck >>"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    make -C "$REPO_ROOT/src/bin/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" tapcheck >>"$LOG_DIR/check_world.log" 2>&1 || CW_OK=false
    stop_local_check_server
    verify_check_server_log "$LOG_DIR/check_server.log" || CW_OK=false
    CHECK_WORLD_STATUS="$( $CW_OK && echo "passed" || echo "failed" )"
elif [[ "$CHECK_WORLD_MODE" == "fast" ]]; then
    log_info "Running fast verification (contrib/pgbench SQL regression + pgbench TAP check)..."
    CW_OK=true
    stop_local_check_server
    mkdir -p "$WORKDIR/tmp"
    rm -f "$LOG_DIR/check_server.log"
    "$PG_INITDB" -D "$CHECK_PGDATA" --waldir="$CHECK_PGWAL" --encoding=UTF8 --locale=C --no-instructions >/dev/null
    {
        echo "port = $CHECK_PGPORT"
        echo "unix_socket_directories = '/tmp'"
    } >> "$CHECK_PGDATA/postgresql.conf"
    "$PG_CTL" -D "$CHECK_PGDATA" -l "$LOG_DIR/check_server.log" -w start >/dev/null
    make -C "$REPO_ROOT/contrib/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" PGPORT="$CHECK_PGPORT" PGHOST="127.0.0.1" installcheck >"$LOG_DIR/fast_check.log" 2>&1 || CW_OK=false
    PROVE_TESTS="t/002_pgbench_no_server.pl" make -C "$REPO_ROOT/src/bin/pgbench" USE_PGXS=1 PG_CONFIG="$TARGET_PG_CONFIG" installcheck >>"$LOG_DIR/fast_check.log" 2>&1 || CW_OK=false
    stop_local_check_server
    verify_check_server_log "$LOG_DIR/check_server.log" || CW_OK=false
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
run_on_server "rm -rf '$PGDATA_DIR' '$PGWAL_DIR' && mkdir -p \"\$(dirname '$PGDATA_DIR')\" \"\$(dirname '$PGWAL_DIR')\""

log_info "Initializing fresh benchmark PostgreSQL cluster on server (data=$PGDATA_DIR, wal=$PGWAL_DIR)..."
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

    python3 - "$PG_CONF_FILE" "$PGPORT" "$guc_overrides_json" > "$WORKDIR/tmp/per_test_override.conf" <<'PY'
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
        run_on_server "rm -f '$SERVER_WORKDIR/postgres_current.log' && '$PG_CTL' -D '$PGDATA_DIR' -l '$SERVER_WORKDIR/postgres_current.log' -w start >/dev/null"
    else
        cp "$WORKDIR/tmp/per_test_override.conf" "$PGDATA_DIR/postgresql.conf"
        rm -f "$test_log"
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

    # 5f. Build & Execute pgbench Benchmark Command (Locally or on Remote Client)
    IS_REMOTE_CLIENT=false
    if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
        IS_REMOTE_CLIENT=true
        CLIENT_TEST_DIR="$CLIENT_WORKDIR/test_${BATCH_ID}_${idx}_${TEST_NAME}"
        run_on_client "rm -rf '$CLIENT_TEST_DIR' && mkdir -p '$CLIENT_TEST_DIR'"
        EFFECTIVE_REPORT_JSON="$CLIENT_TEST_DIR/report.json"
        EFFECTIVE_LOG_PREFIX="$CLIENT_TEST_DIR/pgbench_log"
        CLIENT_REPO_DIR="$(run_on_client "if [[ -d '$CLIENT_WORKDIR/pgbench4perffarm/src/bin/pgbench' ]]; then echo '$CLIENT_WORKDIR/pgbench4perffarm'; else echo '$REPO_ROOT'; fi")"
    else
        EFFECTIVE_REPORT_JSON="$REPORT_JSON"
        EFFECTIVE_LOG_PREFIX="$TEST_DIR/pgbench_log"
        CLIENT_REPO_DIR="$REPO_ROOT"
    fi

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
        --json-report="$EFFECTIVE_REPORT_JSON"
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
            PGBENCH_ARGS+=(-P 1 -l --log-prefix="$EFFECTIVE_LOG_PREFIX" --aggregate-interval=1)
            ;;
        full-trace)
            PGBENCH_ARGS+=(-P 1 -l --log-prefix="$EFFECTIVE_LOG_PREFIX" --log-db-index)
            ;;
    esac

    if [[ -n "$BUILTIN_NAME" ]]; then
        PGBENCH_ARGS+=(-b "$BUILTIN_NAME")
    else
        PGBENCH_ARGS=(--config="$CLIENT_REPO_DIR/$CONFIG_REL" "${PGBENCH_ARGS[@]}")
    fi
    PGBENCH_ARGS+=("$BENCH_DB")

    log_info "[$TEST_NAME] Running benchmark (metrics_level=$METRICS_LEVEL, topology=$TOPOLOGY)..."
    set +e
    if [[ "$IS_REMOTE_CLIENT" == "true" ]]; then
        QUOTED_PGBENCH="$(printf '%q ' "$PGBENCH_BIN" "${PGBENCH_ARGS[@]}")"
        CLIENT_FWD_OPTS=""
        if [[ "${METRICS_DB_HOST:-127.0.0.1}" == "127.0.0.1" || "${METRICS_DB_HOST:-}" == "localhost" ]]; then
            CLIENT_FWD_OPTS="-o ExitOnForwardFailure=no -R ${METRICS_DB_PORT:-55433}:127.0.0.1:${METRICS_DB_PORT:-55433}"
        fi
        # shellcheck disable=SC2086
        ssh $CLIENT_SSH_OPTS $CLIENT_FWD_OPTS "$CLIENT_SSH_TARGET" \
            "cd '$CLIENT_REPO_DIR/src/bin/pgbench' && $QUOTED_PGBENCH" \
            >"$TEST_DIR/pgbench.out" 2>"$TEST_DIR/pgbench.err"
        PGBENCH_EXIT=$?
        # Retrieve report.json and any pgbench_log.* files from remote client
        # shellcheck disable=SC2086
        scp -q $CLIENT_SSH_OPTS "$CLIENT_SSH_TARGET:$EFFECTIVE_REPORT_JSON" "$REPORT_JSON" 2>/dev/null || true
        # shellcheck disable=SC2086
        scp -q $CLIENT_SSH_OPTS "$CLIENT_SSH_TARGET:${EFFECTIVE_LOG_PREFIX}*" "$TEST_DIR/" 2>/dev/null || true
        run_on_client "rm -rf '$CLIENT_TEST_DIR'" >/dev/null 2>&1 || true
    else
        (
            cd "$REPO_ROOT/src/bin/pgbench"
            "$PGBENCH_BIN" "${PGBENCH_ARGS[@]}" >"$TEST_DIR/pgbench.out" 2>"$TEST_DIR/pgbench.err"
        )
        PGBENCH_EXIT=$?
    fi
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
        --upload-level="${UPLOAD_LEVEL:-timeseries}" \
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
    --upload-level="${UPLOAD_LEVEL:-timeseries}" \
    --psql-bin="$METRICS_PSQL"

if [[ "${AUTO_TEARDOWN:-false}" == "true" ]]; then
    bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" teardown
fi

log_info "=================================================================="
log_info "Perffarm Batch $BATCH_ID Complete! Status: $FINAL_BATCH_STATUS"
log_info "  Passed: $TESTS_PASSED | Failed: $TESTS_FAILED | Skipped: $TESTS_SKIPPED"
log_info "  Batch Artifacts: $BATCH_DIR"
log_info "=================================================================="
if [[ "$TESTS_FAILED" -gt 0 || "$FINAL_BATCH_STATUS" == "failed" ]]; then
    exit 1
fi
exit 0
