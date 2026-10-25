#!/usr/bin/env bash
# =============================================================================
# setup_machine.sh — One-Time Perffarm Animal Machine Setup & Discovery
# =============================================================================
# Performs two-tier setup for a pgbench4perffarm animal:
#   1. Default (unprivileged):
#      - Creates workspace, data ($PGDATA_DIR), WAL ($PGWAL_DIR), results,
#        archive, log, and state directories (locally and on remote SSH nodes).
#      - Verifies C compiler, build tools, Perl TAP dependencies, and libraries.
#      - Auto-discovers CPU, RAM, and separate system (/), data ($PGDATA_DIR),
#        and WAL ($PGWAL_DIR) disk characteristics for server and client instances.
#      - Generates deterministic postgresql.animal.conf via generate_pgconf.py.
#   2. Optional privileged flags (--install-deps, --tune-os):
#      - Installs required OS build packages via apt-get or dnf.
#      - Tunes Linux sysctl parameters, CPU frequency governor (performance),
#        and HugePages for dedicated benchmark hosts.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${CONF_FILE:-$SCRIPT_DIR/conf/animal.conf}"

INIT_CONFIG=false
INSTALL_DEPS=false
TUNE_OS=false
IO_PROBE=true
PRESET_OVERRIDE=""
PG_VERSION_OVERRIDE=""

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
        --init-config)
            INIT_CONFIG=true
            shift
            ;;
        --install-deps)
            INSTALL_DEPS=true
            shift
            ;;
        --tune-os)
            TUNE_OS=true
            shift
            ;;
        --no-io-probe)
            IO_PROBE=false
            shift
            ;;
        --preset=*)
            PRESET_OVERRIDE="${1#*=}"
            shift
            ;;
        --pg-version=*)
            PG_VERSION_OVERRIDE="${1#*=}"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --config=PATH        Path to animal.conf (default: perffarm/conf/animal.conf)
  --init-config        Create perffarm/conf/animal.conf from animal.conf.example if missing
  --install-deps       Install PostgreSQL build & TAP test packages via sudo apt-get/dnf
  --tune-os            Tune CPU governor, THP, and Linux vm.* sysctls via sudo
  --no-io-probe        Skip the 16 MB sequential write + fsync disk micro-probe
  --preset=TIER        Override PG_SIZING_PRESET (auto|small-dev|medium-vm|large-baremetal)
  --pg-version=MAJOR   Target PostgreSQL major version for GUC filtering (default: 20)
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

if [[ ! -f "$CONF_FILE" ]]; then
    if [[ "$INIT_CONFIG" == "true" || "$CONF_FILE" == "$SCRIPT_DIR/conf/animal.conf" ]]; then
        echo "[setup_machine] Initializing $CONF_FILE from conf/animal.conf.example..."
        cp "$SCRIPT_DIR/conf/animal.conf.example" "$CONF_FILE"
    else
        echo "[setup_machine] ERROR: Configuration file not found: $CONF_FILE" >&2
        exit 1
    fi
fi

# shellcheck disable=SC1090
source "$CONF_FILE"

if [[ -n "$PRESET_OVERRIDE" ]]; then
    PG_SIZING_PRESET="$PRESET_OVERRIDE"
fi

log_info() {
    echo "[setup_machine] $(date -u '+%Y-%m-%dT%H:%M:%SZ') $*"
}

# 1. Optional Privileged Package Installation (--install-deps)
if [[ "$INSTALL_DEPS" == "true" ]]; then
    log_info "Installing PostgreSQL build & test dependencies (--install-deps)..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            build-essential gcc make git pkg-config bison flex \
            libreadline-dev zlib1g-dev libssl-dev libxml2-dev \
            liblz4-dev libzstd-dev libicu-dev \
            perl libipc-run-perl python3 zstd tar curl
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y \
            gcc make git pkgconfig bison flex \
            readline-devel zlib-devel openssl-devel libxml2-devel \
            lz4-devel libzstd-devel libicu-devel \
            perl perl-IPC-Run python3 zstd tar curl
    else
        echo "[setup_machine] WARNING: Neither apt-get nor dnf found; skipping package installation." >&2
    fi
fi

# 2. Optional Privileged OS Tuning (--tune-os)
if [[ "$TUNE_OS" == "true" ]]; then
    log_info "Applying benchmark OS tuning (--tune-os)..."
    sudo sysctl -w vm.swappiness=1 >/dev/null || true
    sudo sysctl -w vm.dirty_background_ratio=3 >/dev/null || true
    sudo sysctl -w vm.dirty_ratio=10 >/dev/null || true
    sudo sysctl -w kernel.sched_autogroup_enabled=0 >/dev/null || true
    if [[ -n "${HUGEPAGES_NR:-}" ]]; then
        sudo sysctl -w "vm.nr_hugepages=${HUGEPAGES_NR}" >/dev/null || true
    elif [[ -r /proc/meminfo ]]; then
        MEM_KB="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
        if [[ -n "$MEM_KB" && "$MEM_KB" -gt 0 ]]; then
            # Reserve ~28% of RAM in 2 MB HugePages to accommodate shared_buffers (25% of RAM)
            HP_NR=$(( (MEM_KB * 28 / 100) / 2048 + 64 ))
            sudo sysctl -w "vm.nr_hugepages=${HP_NR}" >/dev/null || true
        fi
    fi
    if [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
        for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            [[ -w "$gov" ]] && echo performance | sudo tee "$gov" >/dev/null || true
        done
    fi
    if [[ -f /sys/kernel/mm/transparent_hugepage/enabled ]]; then
        echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null || true
    fi
fi

# 3. Create Local & Remote Workspace Directories
log_info "Creating workspace directories under WORKDIR=$WORKDIR..."
mkdir -p \
    "$WORKDIR/src" \
    "$WORKDIR/build" \
    "$WORKDIR/install" \
    "$PGDATA_DIR" \
    "$PGWAL_DIR" \
    "$METRICS_DB_DATADIR" \
    "$STATE_DIR" \
    "$RESULTS_DIR" \
    "$ARCHIVE_DIR" \
    "$LOG_DIR" \
    "$WORKDIR/tmp"

if [[ "${TOPOLOGY:-local}" == "remote-server" || "${TOPOLOGY:-local}" == "both-remote" ]]; then
    log_info "Preparing remote server workspace on $SERVER_SSH_TARGET..."
    # shellcheck disable=SC2086
    ssh $SERVER_SSH_OPTS "$SERVER_SSH_TARGET" \
        "mkdir -p '$SERVER_WORKDIR' '$PGDATA_DIR' '$PGWAL_DIR'"
fi

if [[ "${TOPOLOGY:-local}" == "remote-client" || "${TOPOLOGY:-local}" == "both-remote" ]]; then
    log_info "Preparing remote client workspace on $CLIENT_SSH_TARGET..."
    # shellcheck disable=SC2086
    ssh $CLIENT_SSH_OPTS "$CLIENT_SSH_TARGET" \
        "mkdir -p '$CLIENT_WORKDIR'"
fi

# 4. Verify Required Build & Runtime Dependencies
log_info "Checking build and runtime toolchain..."
MISSING_TOOLS=()
for tool in "${CC:-gcc}" make git python3 perl tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        MISSING_TOOLS+=("$tool")
    fi
done

if [[ ${#MISSING_TOOLS[@]} -gt 0 ]]; then
    echo "[setup_machine] ERROR: Missing required tools: ${MISSING_TOOLS[*]}" >&2
    echo "Run with --install-deps or install them manually." >&2
    exit 1
fi

# Auto-detect optional library flags if AUTO_DETECT_LIBS=true
DETECTED_LIB_FLAGS=""
if [[ "${AUTO_DETECT_LIBS:-true}" == "true" ]] && command -v pkg-config >/dev/null 2>&1; then
    if [[ "${CONFIGURE_FLAGS:-}" != *"--with-icu"* && "${CONFIGURE_FLAGS:-}" != *"--without-icu"* ]]; then
        if pkg-config --exists icu-uc icu-i18n 2>/dev/null; then
            DETECTED_LIB_FLAGS+=" --with-icu"
        else
            DETECTED_LIB_FLAGS+=" --without-icu"
        fi
    fi
    pkg-config --exists openssl 2>/dev/null && DETECTED_LIB_FLAGS+=" --with-ssl=openssl"
    pkg-config --exists liblz4 2>/dev/null  && DETECTED_LIB_FLAGS+=" --with-lz4"
    pkg-config --exists libzstd 2>/dev/null && DETECTED_LIB_FLAGS+=" --with-zstd"
    pkg-config --exists libxml-2.0 2>/dev/null && DETECTED_LIB_FLAGS+=" --with-libxml"
fi
echo "${CONFIGURE_FLAGS:-} ${DETECTED_LIB_FLAGS}" | xargs > "$STATE_DIR/effective_configure_flags.txt"
log_info "Effective PostgreSQL configure flags: $(cat "$STATE_DIR/effective_configure_flags.txt")"

# 5. Collect Hardware, CPU, RAM & Separate System/Data/WAL Disk Info for Server & Client
PROBE_ARG=""
if [[ "$IO_PROBE" == "true" ]]; then
    PROBE_ARG="--io-probe"
fi
bash "$SCRIPT_DIR/provision_hosts.sh" --config="$CONF_FILE" ${PROBE_ARG:+$PROBE_ARG} collect-info

# 6. Detect Target PostgreSQL Major Version (if PREBUILT_PG_CONFIG is set)
PG_MAJOR="${PG_VERSION_OVERRIDE:-20}"
if [[ -z "$PG_VERSION_OVERRIDE" && -n "${PREBUILT_PG_CONFIG:-}" ]] && command -v "${PREBUILT_PG_CONFIG}" >/dev/null 2>&1; then
    VER_STR="$("$PREBUILT_PG_CONFIG" --version | awk '{print $2}')"
    PG_MAJOR="$(echo "$VER_STR" | sed -E 's/^([0-9]+).*/\1/')"
fi

# 7. Generate Deterministic postgresql.animal.conf
log_info "Generating deterministic server config ($PG_CONF_FILE) with preset=${PG_SIZING_PRESET:-auto} (PG $PG_MAJOR)..."
python3 "$SCRIPT_DIR/generate_pgconf.py" \
    --hardware-profile="$STATE_DIR/hardware_profile.json" \
    --animal-name="$ANIMAL_NAME" \
    --preset="${PG_SIZING_PRESET:-auto}" \
    --pg-version="$PG_MAJOR" \
    --max-connections="${PG_MAX_CONNECTIONS:-200}" \
    --extra-gucs="${PG_EXTRA_GUCS:-}" \
    --output="$PG_CONF_FILE"

log_info "Machine setup complete!"
log_info "  Hardware Profile : $STATE_DIR/hardware_profile.json"
log_info "  Server Info      : $STATE_DIR/server_instance_info.json"
log_info "  Client Info      : $STATE_DIR/client_instance_info.json"
log_info "  PostgreSQL Conf  : $PG_CONF_FILE"
