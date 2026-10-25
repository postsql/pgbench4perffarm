#!/usr/bin/env python3
"""
collect_pg_stats.py — Pre/Post PostgreSQL catalog and OS /proc snapshot collector
and delta calculator for pgbench4perffarm.

Captures:
  - pg_stat_database (for target database)
  - pg_stat_bgwriter
  - pg_stat_checkpointer (PostgreSQL 17+, graceful empty object on PG 14-16)
  - pg_stat_wal
  - pg_stat_io (PostgreSQL 16+, aggregated by backend_type/object/context)
  - pg_stat_statements (top 25 statements by total_exec_time if extension loaded)
  - pg_settings (non-default server settings)
  - pg_database_size(current_database())
  - Linux /proc/stat & /proc/vmstat & /proc/meminfo on server and client instances
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shlex
import subprocess
import sys


def run_psql_json(psql_bin: str, host: str, port: str, user: str, dbname: str, sql: str):
    cmd = [psql_bin, "-X", "-q", "-t", "-A"]
    if host:
        cmd.extend(["-h", host])
    if port:
        cmd.extend(["-p", str(port)])
    if user:
        cmd.extend(["-U", user])
    cmd.extend(["-d", dbname, "-c", sql])
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if res.returncode != 0 or not res.stdout.strip():
        return None
    try:
        return json.loads(res.stdout.strip().splitlines()[-1])
    except Exception:
        return None


def collect_proc_snapshot_local() -> dict:
    cpu_jiffies = {}
    ctxt = 0
    procs_running = 0
    try:
        with open("/proc/stat", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("cpu "):
                    parts = [int(x) for x in line.split()[1:9]]
                    while len(parts) < 8:
                        parts.append(0)
                    cpu_jiffies = {
                        "user": parts[0],
                        "nice": parts[1],
                        "system": parts[2],
                        "idle": parts[3],
                        "iowait": parts[4],
                        "irq": parts[5],
                        "softirq": parts[6],
                        "steal": parts[7],
                    }
                elif line.startswith("ctxt "):
                    ctxt = int(line.split()[1])
                elif line.startswith("procs_running "):
                    procs_running = int(line.split()[1])
    except Exception:
        pass

    vmstat = {}
    try:
        with open("/proc/vmstat", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2 and parts[0] in ("pgpgin", "pgpgout", "pswpin", "pswpout"):
                    vmstat[parts[0]] = int(parts[1])
    except Exception:
        pass

    mem = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if ":" in line:
                    k, v = line.split(":", 1)
                    if k.strip() in ("MemTotal", "MemAvailable", "Dirty", "Writeback", "Cached"):
                        toks = v.strip().split()
                        if toks and toks[0].isdigit():
                            mem[k.strip()] = int(toks[0]) * 1024
    except Exception:
        pass

    loadavg = [0.0, 0.0, 0.0]
    try:
        with open("/proc/loadavg", "r", encoding="utf-8", errors="replace") as f:
            parts = f.read().split()
            if len(parts) >= 3:
                loadavg = [float(parts[0]), float(parts[1]), float(parts[2])]
    except Exception:
        pass

    return {
        "cpu_jiffies": cpu_jiffies,
        "ctxt": ctxt,
        "procs_running": procs_running,
        "vmstat": vmstat,
        "mem_bytes": mem,
        "loadavg": loadavg,
    }


def collect_proc_snapshot(ssh_target: str = "", ssh_opts: str = "") -> dict:
    if not ssh_target:
        return collect_proc_snapshot_local()
    py_one_liner = (
        "import json; "
        "f_stat=open('/proc/stat').read().splitlines(); "
        "cpu=[int(x) for l in f_stat if l.startswith('cpu ') for x in l.split()[1:9]]; "
        "ctxt=next((int(l.split()[1]) for l in f_stat if l.startswith('ctxt ')),0); "
        "vm={l.split()[0]:int(l.split()[1]) for l in open('/proc/vmstat') if l.split()[0] in ('pgpgin','pgpgout','pswpin','pswpout')}; "
        "la=[float(x) for x in open('/proc/loadavg').read().split()[:3]]; "
        "print(json.dumps({'cpu_jiffies':dict(zip(['user','nice','system','idle','iowait','irq','softirq','steal'],cpu)),'ctxt':ctxt,'vmstat':vm,'loadavg':la}))"
    )
    cmd = ["ssh"]
    if ssh_opts:
        cmd.extend(shlex.split(ssh_opts))
    cmd.extend([ssh_target, f"python3 -c {shlex.quote(py_one_liner)}"])
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20, check=False)
    if res.returncode == 0 and res.stdout.strip():
        try:
            return json.loads(res.stdout.strip().splitlines()[-1])
        except Exception:
            pass
    return {}


def compute_proc_delta(pre: dict, post: dict) -> dict:
    if not pre or not post:
        return {}
    c1 = pre.get("cpu_jiffies", {})
    c2 = post.get("cpu_jiffies", {})
    diffs = {k: max(0, int(c2.get(k, 0)) - int(c1.get(k, 0))) for k in c2}
    total_jiffies = sum(diffs.values())
    cpu_pct = {}
    if total_jiffies > 0:
        cpu_pct = {
            "user_pct": round(100.0 * (diffs.get("user", 0) + diffs.get("nice", 0)) / total_jiffies, 2),
            "system_pct": round(
                100.0 * (diffs.get("system", 0) + diffs.get("irq", 0) + diffs.get("softirq", 0)) / total_jiffies,
                2,
            ),
            "iowait_pct": round(100.0 * diffs.get("iowait", 0) / total_jiffies, 2),
            "steal_pct": round(100.0 * diffs.get("steal", 0) / total_jiffies, 2),
            "idle_pct": round(100.0 * diffs.get("idle", 0) / total_jiffies, 2),
            "busy_pct": round(100.0 * (total_jiffies - diffs.get("idle", 0)) / total_jiffies, 2),
        }
    v1 = pre.get("vmstat", {})
    v2 = post.get("vmstat", {})
    vm_delta = {k: max(0, int(v2.get(k, 0)) - int(v1.get(k, 0))) for k in v2}
    return {
        "cpu_utilization_pct": cpu_pct,
        "context_switches_delta": max(0, int(post.get("ctxt", 0)) - int(pre.get("ctxt", 0))),
        "vmstat_kb_delta": vm_delta,
        "post_mem_bytes": post.get("mem_bytes", {}),
        "post_loadavg": post.get("loadavg", []),
    }


def diff_numeric_dict(d_pre: dict | None, d_post: dict | None, skip_keys: set[str] | None = None) -> dict:
    if not d_post:
        return {}
    if not d_pre:
        return dict(d_post)
    skip = skip_keys or set()
    out = {}
    for k, v2 in d_post.items():
        if k in skip:
            out[k] = v2
            continue
        v1 = d_pre.get(k)
        if isinstance(v2, (int, float)) and not isinstance(v2, bool) and isinstance(v1, (int, float)):
            out[k] = round(v2 - v1, 4) if isinstance(v2, float) else (v2 - v1)
        else:
            out[k] = v2
    return out


def do_snapshot(args: argparse.Namespace) -> int:
    ver_num = run_psql_json(
        args.psql_bin,
        args.host,
        args.port,
        args.user,
        args.dbname,
        "SELECT current_setting('server_version_num')::int;",
    ) or 200000

    db_size = run_psql_json(
        args.psql_bin,
        args.host,
        args.port,
        args.user,
        args.dbname,
        "SELECT pg_database_size(current_database());",
    ) or 0

    stat_db = run_psql_json(
        args.psql_bin,
        args.host,
        args.port,
        args.user,
        args.dbname,
        "SELECT row_to_json(d) FROM pg_stat_database d WHERE d.datname = current_database();",
    ) or {}

    stat_bgw = run_psql_json(
        args.psql_bin,
        args.host,
        args.port,
        args.user,
        args.dbname,
        "SELECT row_to_json(b) FROM pg_stat_bgwriter b;",
    ) or {}

    stat_ckpt = {}
    if int(ver_num) >= 170000:
        stat_ckpt = run_psql_json(
            args.psql_bin,
            args.host,
            args.port,
            args.user,
            args.dbname,
            "SELECT row_to_json(c) FROM pg_stat_checkpointer c;",
        ) or {}

    stat_wal = run_psql_json(
        args.psql_bin,
        args.host,
        args.port,
        args.user,
        args.dbname,
        "SELECT row_to_json(w) FROM pg_stat_wal w;",
    ) or {}

    stat_io = []
    if int(ver_num) >= 160000:
        stat_io = run_psql_json(
            args.psql_bin,
            args.host,
            args.port,
            args.user,
            args.dbname,
            "SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM ("
            "  SELECT backend_type, object, context, "
            "         COALESCE(reads, 0) AS reads, COALESCE(read_time, 0) AS read_time, "
            "         COALESCE(writes, 0) AS writes, COALESCE(write_time, 0) AS write_time, "
            "         COALESCE(extends, 0) AS extends, COALESCE(extend_time, 0) AS extend_time, "
            "         COALESCE(hits, 0) AS hits, COALESCE(evictions, 0) AS evictions, "
            "         COALESCE(fsyncs, 0) AS fsyncs, COALESCE(fsync_time, 0) AS fsync_time "
            "  FROM pg_stat_io "
            "  WHERE COALESCE(reads, 0) + COALESCE(writes, 0) + COALESCE(extends, 0) + COALESCE(hits, 0) + COALESCE(fsyncs, 0) > 0"
            ") t;",
        ) or []

    stat_stmts = []
    if args.phase == "post":
        has_pgss = run_psql_json(
            args.psql_bin,
            args.host,
            args.port,
            args.user,
            args.dbname,
            "SELECT count(*) FROM pg_extension WHERE extname = 'pg_stat_statements';",
        ) or 0
        if int(has_pgss) > 0:
            stat_stmts = run_psql_json(
                args.psql_bin,
                args.host,
                args.port,
                args.user,
                args.dbname,
                "SELECT COALESCE(json_agg(row_to_json(s)), '[]'::json) FROM ("
                "  SELECT left(query, 240) AS query, calls, rows, "
                "         round(total_exec_time::numeric, 3) AS total_exec_time_ms, "
                "         round(mean_exec_time::numeric, 3) AS mean_exec_time_ms, "
                "         shared_blks_hit, shared_blks_read, shared_blks_dirtied, shared_blks_written, "
                "         wal_records, wal_bytes "
                "  FROM pg_stat_statements "
                "  WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) "
                "  ORDER BY total_exec_time DESC LIMIT 20"
                ") s;",
            ) or []

    settings = {}
    if args.phase == "post":
        settings = run_psql_json(
            args.psql_bin,
            args.host,
            args.port,
            args.user,
            args.dbname,
            "SELECT COALESCE(json_object_agg(name, json_build_object('setting', setting, 'unit', unit, 'source', source)), '{}'::json) "
            "FROM pg_settings WHERE source NOT IN ('default', 'client') OR name IN ("
            "'shared_buffers','work_mem','maintenance_work_mem','effective_cache_size',"
            "'max_connections','max_wal_size','min_wal_size','checkpoint_timeout',"
            "'checkpoint_completion_target','synchronous_commit','wal_compression',"
            "'random_page_cost','effective_io_concurrency','max_parallel_workers_per_gather',"
            "'track_io_timing','track_wal_io_timing','jit');",
        ) or {}

    server_proc = collect_proc_snapshot(args.server_ssh_target, args.ssh_opts)
    client_proc = collect_proc_snapshot(args.client_ssh_target, args.ssh_opts)

    payload = {
        "phase": args.phase,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "server_version_num": int(ver_num),
        "db_size_bytes": int(db_size),
        "pg_stat_database": stat_db,
        "pg_stat_bgwriter": stat_bgw,
        "pg_stat_checkpointer": stat_ckpt,
        "pg_stat_wal": stat_wal,
        "pg_stat_io": stat_io,
        "pg_stat_statements_top": stat_stmts,
        "server_settings": settings,
        "server_proc": server_proc,
        "client_proc": client_proc,
    }

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return 0


def do_delta(args: argparse.Namespace) -> int:
    pre = json.loads(Path(args.pre).read_text(encoding="utf-8"))
    post = json.loads(Path(args.post).read_text(encoding="utf-8"))

    skip_meta = {"datid", "datname", "stats_reset"}
    db_delta = diff_numeric_dict(pre.get("pg_stat_database"), post.get("pg_stat_database"), skip_meta)
    blks_hit = max(0, int(db_delta.get("blks_hit") or 0))
    blks_read = max(0, int(db_delta.get("blks_read") or 0))
    if blks_hit + blks_read > 0:
        db_delta["buffer_hit_ratio_pct"] = round(100.0 * blks_hit / (blks_hit + blks_read), 4)
    else:
        db_delta["buffer_hit_ratio_pct"] = 100.0

    bgw_delta = diff_numeric_dict(pre.get("pg_stat_bgwriter"), post.get("pg_stat_bgwriter"), {"stats_reset"})
    ckpt_delta = diff_numeric_dict(pre.get("pg_stat_checkpointer"), post.get("pg_stat_checkpointer"), {"stats_reset"})
    wal_delta = diff_numeric_dict(pre.get("pg_stat_wal"), post.get("pg_stat_wal"), {"stats_reset"})

    # Compute pg_stat_io delta keyed by (backend_type, object, context)
    pre_io_map = {
        (r.get("backend_type"), r.get("object"), r.get("context")): r
        for r in (pre.get("pg_stat_io") or [])
    }
    io_deltas = []
    for r_post in post.get("pg_stat_io") or []:
        key = (r_post.get("backend_type"), r_post.get("object"), r_post.get("context"))
        r_pre = pre_io_map.get(key, {})
        d_row = diff_numeric_dict(r_pre, r_post, {"backend_type", "object", "context"})
        total_act = (
            abs(int(d_row.get("reads") or 0))
            + abs(int(d_row.get("writes") or 0))
            + abs(int(d_row.get("extends") or 0))
            + abs(int(d_row.get("hits") or 0))
            + abs(int(d_row.get("fsyncs") or 0))
        )
        if total_act > 0:
            io_deltas.append(d_row)

    os_metrics = compute_proc_delta(pre.get("server_proc", {}), post.get("server_proc", {}))
    client_os_metrics = compute_proc_delta(pre.get("client_proc", {}), post.get("client_proc", {}))

    delta_payload = {
        "server_version_num": post.get("server_version_num"),
        "db_size_bytes_pre": pre.get("db_size_bytes", 0),
        "db_size_bytes": post.get("db_size_bytes", 0),
        "db_size_growth_bytes": int(post.get("db_size_bytes", 0)) - int(pre.get("db_size_bytes", 0)),
        "server_settings": post.get("server_settings", {}),
        "pg_stat_database_delta": db_delta,
        "pg_stat_bgwriter_delta": bgw_delta,
        "pg_stat_checkpointer_delta": ckpt_delta,
        "pg_stat_wal_delta": wal_delta,
        "pg_stat_io_delta": io_deltas,
        "pg_stat_statements_top": post.get("pg_stat_statements_top", []),
        "os_metrics": os_metrics,
        "client_os_metrics": client_os_metrics,
    }

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(delta_payload, indent=2) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture pre/post pg_stat_* and OS snapshots and compute deltas.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_snap = sub.add_parser("snapshot", help="Capture a pre or post snapshot")
    p_snap.add_argument("--phase", choices=["pre", "post"], required=True)
    p_snap.add_argument("--psql-bin", default="psql")
    p_snap.add_argument("--host", default="127.0.0.1")
    p_snap.add_argument("--port", default="5432")
    p_snap.add_argument("--user", default="")
    p_snap.add_argument("--dbname", required=True)
    p_snap.add_argument("--server-ssh-target", default="")
    p_snap.add_argument("--client-ssh-target", default="")
    p_snap.add_argument("--ssh-opts", default="-o BatchMode=yes -o StrictHostKeyChecking=accept-new")
    p_snap.add_argument("--output", required=True)

    p_delta = sub.add_parser("delta", help="Compute delta between pre and post snapshots")
    p_delta.add_argument("--pre", required=True)
    p_delta.add_argument("--post", required=True)
    p_delta.add_argument("--output", required=True)

    args = parser.parse_args()
    if args.cmd == "snapshot":
        return do_snapshot(args)
    return do_delta(args)


if __name__ == "__main__":
    sys.exit(main())
