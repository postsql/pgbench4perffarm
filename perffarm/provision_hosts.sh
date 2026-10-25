#!/usr/bin/env bash
# =============================================================================
# provision_hosts.sh — Provision, verify, sync, or tear down local or external
# Cloud VM instances (server and/or client) for pgbench4perffarm.
#
# Supports all 4 topologies:
#   - TOPOLOGY="local"         : Both PostgreSQL server and pgbench client local
#   - TOPOLOGY="remote-server" : PostgreSQL server on external/Cloud VM, client local
#   - TOPOLOGY="remote-client" : PostgreSQL server local, pgbench client on external/Cloud VM
#   - TOPOLOGY="both-remote"   : Both server and client on external/Cloud VMs
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONF_FILE="${CONF_FILE:-$SCRIPT_DIR/conf/animal.conf}"

ACTION="status"
IO_PROBE_FLAG=""

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
        --io-probe)
            IO_PROBE_FLAG="--io-probe"
            shift
            ;;
        provision|verify-ssh|sync-repo|collect-info|teardown|status)
            ACTION="$1"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS] <provision|verify-ssh|sync-repo|collect-info|teardown|status>

Options:
  --config=PATH   Path to animal.conf (default: perffarm/conf/animal.conf)
  --io-probe      Run 16 MB sequential write + fsync micro-probe on system/data/WAL disks
  -h, --help      Show this help message
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -f "$CONF_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$CONF_FILE"
elif [[ -f "$SCRIPT_DIR/conf/animal.conf.example" ]]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/conf/animal.conf.example"
fi

TOPOLOGY="${TOPOLOGY:-local}"
CLOUD_PROVIDER="${CLOUD_PROVIDER:-none}"
WORKDIR="${WORKDIR:-/var/tmp/pgbench4perffarm_work}"
STATE_DIR="${STATE_DIR:-$WORKDIR/state}"
PGDATA_DIR="${PGDATA_DIR:-$WORKDIR/pgdata}"
PGWAL_DIR="${PGWAL_DIR:-$WORKDIR/pgwal}"
SERVER_WORKDIR="${SERVER_WORKDIR:-/var/tmp/perffarm_server}"
CLIENT_WORKDIR="${CLIENT_WORKDIR:-/var/tmp/perffarm_client}"
SERVER_SSH_OPTS="${SERVER_SSH_OPTS:--o BatchMode=yes -o StrictHostKeyChecking=accept-new}"
CLIENT_SSH_OPTS="${CLIENT_SSH_OPTS:--o BatchMode=yes -o StrictHostKeyChecking=accept-new}"

mkdir -p "$STATE_DIR"

log_info() {
    echo "[provision_hosts] $(date -u '+%Y-%m-%dT%H:%M:%SZ') $*"
}

wait_for_ssh() {
    local target="$1"
    local opts="$2"
    local role="$3"
    log_info "Verifying SSH connectivity to $role instance ($target)..."
    local attempt=1
    while [[ $attempt -le 30 ]]; do
        # shellcheck disable=SC2086
        if ssh $opts "$target" "echo ok" >/dev/null 2>&1; then
            log_info "SSH ready on $role ($target)"
            return 0
        fi
        sleep 2
        attempt=$((attempt + 1))
    done
    echo "[provision_hosts] ERROR: Timed out waiting for SSH on $role ($target)" >&2
    return 1
}

do_provision() {
    log_info "Provisioning topology=$TOPOLOGY (cloud_provider=$CLOUD_PROVIDER)..."
    case "$CLOUD_PROVIDER" in
        none|ssh)
            log_info "Using existing local/SSH instances (no cloud VM creation needed)."
            ;;
        gcp)
            [[ -n "${GCP_PROJECT:-}" ]] || { echo "GCP_PROJECT required for CLOUD_PROVIDER=gcp" >&2; exit 1; }
            if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
                log_info "Creating GCP server VM ${GCP_SERVER_INSTANCE} in ${GCP_ZONE}..."
                gcloud compute instances create "${GCP_SERVER_INSTANCE}" \
                    --project="${GCP_PROJECT}" \
                    --zone="${GCP_ZONE}" \
                    --machine-type="${GCP_MACHINE_TYPE}" \
                    --image-family="${GCP_IMAGE_FAMILY}" \
                    --image-project="${GCP_IMAGE_PROJECT}" \
                    --boot-disk-size="${GCP_BOOT_DISK_GB}GB" \
                    --create-disk="name=${GCP_SERVER_INSTANCE}-data,size=${GCP_DATA_DISK_GB}GB,type=${GCP_DATA_DISK_TYPE},auto-delete=yes" \
                    --create-disk="name=${GCP_SERVER_INSTANCE}-wal,size=${GCP_WAL_DISK_GB}GB,type=${GCP_WAL_DISK_TYPE},auto-delete=yes"
            fi
            if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
                if [[ "${GCP_CLIENT_INSTANCE}" != "${GCP_SERVER_INSTANCE}" || "$TOPOLOGY" == "remote-client" ]]; then
                    log_info "Creating GCP client VM ${GCP_CLIENT_INSTANCE} in ${GCP_ZONE}..."
                    gcloud compute instances create "${GCP_CLIENT_INSTANCE}" \
                        --project="${GCP_PROJECT}" \
                        --zone="${GCP_ZONE}" \
                        --machine-type="${GCP_MACHINE_TYPE}" \
                        --image-family="${GCP_IMAGE_FAMILY}" \
                        --image-project="${GCP_IMAGE_PROJECT}" \
                        --boot-disk-size="${GCP_BOOT_DISK_GB}GB"
                fi
            fi
            ;;
        aws)
            [[ -n "${AWS_AMI_ID:-}" ]] || { echo "AWS_AMI_ID required for CLOUD_PROVIDER=aws" >&2; exit 1; }
            log_info "Launching AWS EC2 instance(s) in ${AWS_REGION} (${AWS_INSTANCE_TYPE})..."
            aws ec2 run-instances \
                --region "${AWS_REGION}" \
                --image-id "${AWS_AMI_ID}" \
                --instance-type "${AWS_INSTANCE_TYPE}" \
                ${AWS_KEY_NAME:+--key-name "$AWS_KEY_NAME"} \
                ${AWS_SECURITY_GROUP:+--security-group-ids "$AWS_SECURITY_GROUP"} \
                ${AWS_SUBNET_ID:+--subnet-id "$AWS_SUBNET_ID"} \
                --count 1 >/dev/null
            ;;
        custom)
            if [[ -n "${PROVISION_CMD:-}" ]]; then
                log_info "Running custom PROVISION_CMD..."
                eval "$PROVISION_CMD"
            fi
            ;;
        *)
            echo "Unsupported CLOUD_PROVIDER: $CLOUD_PROVIDER" >&2
            exit 1
            ;;
    esac
    do_verify_ssh
}

do_verify_ssh() {
    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        [[ -n "${SERVER_SSH_TARGET:-}" ]] || { echo "SERVER_SSH_TARGET must be set for TOPOLOGY=$TOPOLOGY" >&2; exit 1; }
        wait_for_ssh "$SERVER_SSH_TARGET" "$SERVER_SSH_OPTS" "server"
    fi
    if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
        [[ -n "${CLIENT_SSH_TARGET:-}" ]] || { echo "CLIENT_SSH_TARGET must be set for TOPOLOGY=$TOPOLOGY" >&2; exit 1; }
        wait_for_ssh "$CLIENT_SSH_TARGET" "$CLIENT_SSH_OPTS" "client"
    fi
}

sync_to_target() {
    local target="$1"
    local opts="$2"
    local remote_dir="$3"
    local role="$4"
    log_info "Syncing pgbench4perffarm repository to $role ($target:$remote_dir/pgbench4perffarm)..."
    # shellcheck disable=SC2086
    ssh $opts "$target" "mkdir -p '$remote_dir/pgbench4perffarm'"
    tar -C "$REPO_ROOT" --exclude='.git' -cf - . | \
        # shellcheck disable=SC2086
        ssh $opts "$target" "tar -C '$remote_dir/pgbench4perffarm' -xf -"
}

do_sync_repo() {
    do_verify_ssh
    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        sync_to_target "$SERVER_SSH_TARGET" "$SERVER_SSH_OPTS" "$SERVER_WORKDIR" "server"
    fi
    if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
        if [[ "${CLIENT_SSH_TARGET:-}" != "${SERVER_SSH_TARGET:-}" || "$CLIENT_WORKDIR" != "$SERVER_WORKDIR" ]]; then
            sync_to_target "$CLIENT_SSH_TARGET" "$CLIENT_SSH_OPTS" "$CLIENT_WORKDIR" "client"
        fi
    fi
}

do_collect_info() {
    local srv_out="$STATE_DIR/server_instance_info.json"
    local cli_out="$STATE_DIR/client_instance_info.json"
    local hw_out="$STATE_DIR/hardware_profile.json"

    if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
        log_info "Collecting remote server instance hardware/disk info from $SERVER_SSH_TARGET..."
        python3 "$SCRIPT_DIR/collect_instance_info.py" \
            --role=server \
            --ssh-target="$SERVER_SSH_TARGET" \
            --ssh-opts="$SERVER_SSH_OPTS" \
            --data-dir="$PGDATA_DIR" \
            --wal-dir="$PGWAL_DIR" \
            ${IO_PROBE_FLAG:+$IO_PROBE_FLAG} \
            --output="$srv_out"
    else
        log_info "Collecting local server instance hardware/disk info..."
        python3 "$SCRIPT_DIR/collect_instance_info.py" \
            --role=server \
            --data-dir="$PGDATA_DIR" \
            --wal-dir="$PGWAL_DIR" \
            ${IO_PROBE_FLAG:+$IO_PROBE_FLAG} \
            --output="$srv_out"
    fi

    if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
        log_info "Collecting remote client instance hardware/disk info from $CLIENT_SSH_TARGET..."
        python3 "$SCRIPT_DIR/collect_instance_info.py" \
            --role=client \
            --ssh-target="$CLIENT_SSH_TARGET" \
            --ssh-opts="$CLIENT_SSH_OPTS" \
            --data-dir="$CLIENT_WORKDIR" \
            --wal-dir="$CLIENT_WORKDIR" \
            ${IO_PROBE_FLAG:+$IO_PROBE_FLAG} \
            --output="$cli_out"
    elif [[ "$TOPOLOGY" == "local" ]]; then
        cp "$srv_out" "$cli_out"
    else
        log_info "Collecting local client instance hardware/disk info..."
        python3 "$SCRIPT_DIR/collect_instance_info.py" \
            --role=client \
            --data-dir="$WORKDIR" \
            --wal-dir="$WORKDIR" \
            ${IO_PROBE_FLAG:+$IO_PROBE_FLAG} \
            --output="$cli_out"
    fi

    python3 - "$ANIMAL_NAME" "$TOPOLOGY" "$srv_out" "$cli_out" "$hw_out" <<'PY'
import json
import sys
from datetime import datetime, timezone

animal_name, topology, srv_path, cli_path, out_path = sys.argv[1:6]
srv = json.loads(open(srv_path, encoding="utf-8").read())
cli = json.loads(open(cli_path, encoding="utf-8").read())

combined = {
    "animal_name": animal_name,
    "topology": topology,
    "collected_at_utc": datetime.now(timezone.utc).isoformat(),
    "server_instance_info": srv,
    "client_instance_info": cli,
    # Top-level aliases for backward-compatible tooling
    "hostname": srv.get("hostname"),
    "cpu": srv.get("cpu"),
    "memory": srv.get("memory"),
    "disks": srv.get("disks"),
    "os": srv.get("os"),
    "toolchain": srv.get("toolchain"),
}
open(out_path, "w", encoding="utf-8").write(json.dumps(combined, indent=2) + "\n")
PY
    log_info "Saved server_instance_info.json, client_instance_info.json, and hardware_profile.json in $STATE_DIR"
}

do_teardown() {
    log_info "Tearing down provisioned external resources (cloud_provider=$CLOUD_PROVIDER)..."
    case "$CLOUD_PROVIDER" in
        none|ssh)
            log_info "No dynamic cloud VMs to delete for CLOUD_PROVIDER=$CLOUD_PROVIDER."
            ;;
        gcp)
            if [[ "$TOPOLOGY" == "remote-server" || "$TOPOLOGY" == "both-remote" ]]; then
                gcloud compute instances delete "${GCP_SERVER_INSTANCE}" \
                    --project="${GCP_PROJECT}" --zone="${GCP_ZONE}" --quiet || true
            fi
            if [[ "$TOPOLOGY" == "remote-client" || "$TOPOLOGY" == "both-remote" ]]; then
                if [[ "${GCP_CLIENT_INSTANCE}" != "${GCP_SERVER_INSTANCE}" || "$TOPOLOGY" == "remote-client" ]]; then
                    gcloud compute instances delete "${GCP_CLIENT_INSTANCE}" \
                        --project="${GCP_PROJECT}" --zone="${GCP_ZONE}" --quiet || true
                fi
            fi
            ;;
        custom)
            if [[ -n "${TEARDOWN_CMD:-}" ]]; then
                eval "$TEARDOWN_CMD"
            fi
            ;;
        *)
            log_info "Teardown completed."
            ;;
    esac
}

do_status() {
    echo "=== Perffarm Topology & Host Status ==="
    echo "  Animal Name       : ${ANIMAL_NAME:-unset}"
    echo "  Topology          : ${TOPOLOGY}"
    echo "  Cloud Provider    : ${CLOUD_PROVIDER}"
    echo "  Server Endpoint   : ${SERVER_HOST:-127.0.0.1}:${PGPORT:-55432} (SSH: ${SERVER_SSH_TARGET:-local})"
    echo "  Client Endpoint   : ${CLIENT_HOST:-127.0.0.1} (SSH: ${CLIENT_SSH_TARGET:-local})"
    echo "  Server PGDATA_DIR : ${PGDATA_DIR}"
    echo "  Server PGWAL_DIR  : ${PGWAL_DIR}"
    echo "  State Dir         : ${STATE_DIR}"
}

case "$ACTION" in
    provision)
        do_provision
        ;;
    verify-ssh)
        do_verify_ssh
        ;;
    sync-repo)
        do_sync_repo
        ;;
    collect-info)
        do_collect_info
        ;;
    teardown)
        do_teardown
        ;;
    status)
        do_status
        ;;
esac
