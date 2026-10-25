#!/usr/bin/env python3
"""
calibrate_baseline.py — Client-to-server baseline latency & throughput calibration
for pgbench4perffarm.

Measures and records from the pgbench client instance to the PostgreSQL server instance:
  1. `SELECT 1;`
     - Client-server round-trip latency (avg, min, p50, p90, p95, p99, max in ms & us)
     - Single-client round-trip QPS / TPS
  2. `SELECT repeat('0123456789', 100000);` (1,000,000-byte / 1 MB payload per query)
     - Client-server bulk result transfer latency (avg, min, p50, p90, p95, p99, max in ms)
     - Rows/sec and effective network/IPC throughput in MB/s, MiB/s, and Gbps

Supports running locally on the client host or remotely on an external/Cloud VM
client via --client-ssh-target.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile
import time


SELECT_1_SQL = "SELECT 1;\n"
REPEAT_1MB_SQL = "SELECT repeat('0123456789', 100000);\n"
PAYLOAD_BYTES = 1_000_000  # 100,000 * 10 bytes ('0123456789')


def percentile_from_sorted(vals: list[float], pct: float) -> float:
    if not vals:
        return 0.0
    idx = (pct / 100.0) * (len(vals) - 1)
    lo = int(idx)
    hi = min(len(vals) - 1, lo + 1)
    frac = idx - lo
    return vals[lo] * (1.0 - frac) + vals[hi] * frac


def run_pgbench_probe(
    pgbench_bin: str,
    host: str,
    port: str,
    user: str,
    dbname: str,
    sql_text: str,
    iterations: int,
) -> dict:
    with tempfile.TemporaryDirectory(prefix="perffarm_cal_") as tmpdir:
        sql_file = Path(tmpdir) / "probe.sql"
        rep_file = Path(tmpdir) / "report.json"
        sql_file.write_text(sql_text, encoding="utf-8")

        # Warmup (5 transactions)
        warm_cmd = [
            pgbench_bin,
            "-n",
            "-c",
            "1",
            "-j",
            "1",
            "-t",
            "5",
            "-f",
            str(sql_file),
        ]
        if host:
            warm_cmd.extend(["-h", host])
        if port:
            warm_cmd.extend(["-p", str(port)])
        if user:
            warm_cmd.extend(["-U", user])
        warm_cmd.append(dbname)
        subprocess.run(warm_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)

        # Timed measurement run with HDR histogram JSON report
        cmd = [
            pgbench_bin,
            "-n",
            "-c",
            "1",
            "-j",
            "1",
            "-t",
            str(iterations),
            "-f",
            str(sql_file),
            f"--json-report={rep_file}",
        ]
        if host:
            cmd.extend(["-h", host])
        if port:
            cmd.extend(["-p", str(port)])
        if user:
            cmd.extend(["-U", user])
        cmd.append(dbname)

        t0 = time.perf_counter()
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
        wall_sec = max(1e-6, time.perf_counter() - t0)

        if res.returncode != 0 or not rep_file.exists():
            raise RuntimeError(
                f"pgbench calibration probe failed (exit {res.returncode}): {res.stderr.strip()}"
            )

        rep = json.loads(rep_file.read_text(encoding="utf-8"))
        overall = rep.get("overall", {})
        lat = overall.get("latency_ms", {})
        dur_sec = float(rep.get("duration_sec") or wall_sec)
        tx_cnt = int(overall.get("transactions") or iterations)
        tps = float(overall.get("tps") or (tx_cnt / max(1e-6, dur_sec)))

        avg_ms = float(lat.get("avg") or (1000.0 / max(1e-6, tps)))
        min_ms = float(lat.get("min") or avg_ms)
        max_ms = float(lat.get("max") or avg_ms)
        p50_ms = float(lat.get("p50") or avg_ms)
        p90_ms = float(lat.get("p90") or avg_ms)
        p95_ms = float(lat.get("p95") or avg_ms)
        p99_ms = float(lat.get("p99") or avg_ms)

        return {
            "driver": "pgbench_hdr",
            "iterations": tx_cnt,
            "elapsed_sec": round(dur_sec, 6),
            "qps": round(tps, 2),
            "latency_avg_ms": round(avg_ms, 4),
            "latency_min_ms": round(min_ms, 4),
            "latency_p50_ms": round(p50_ms, 4),
            "latency_p90_ms": round(p90_ms, 4),
            "latency_p95_ms": round(p95_ms, 4),
            "latency_p99_ms": round(p99_ms, 4),
            "latency_max_ms": round(max_ms, 4),
            "latency_avg_us": round(avg_ms * 1000.0, 2),
            "latency_p50_us": round(p50_ms * 1000.0, 2),
            "latency_p95_us": round(p95_ms * 1000.0, 2),
        }


def run_psql_fallback_probe(
    psql_bin: str,
    host: str,
    port: str,
    user: str,
    dbname: str,
    sql_text: str,
    iterations: int,
) -> dict:
    base_cmd = [psql_bin, "-X", "-q", "-t", "-A"]
    if host:
        base_cmd.extend(["-h", host])
    if port:
        base_cmd.extend(["-p", str(port)])
    if user:
        base_cmd.extend(["-U", user])
    base_cmd.extend(["-d", dbname])

    # Run all iterations in a single psql session to avoid fork/connect overhead
    batch_sql = sql_text * iterations
    t0 = time.perf_counter()
    res = subprocess.run(
        base_cmd,
        input=batch_sql,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    elapsed = max(1e-6, time.perf_counter() - t0)
    if res.returncode != 0:
        raise RuntimeError(f"psql calibration probe failed: {res.stderr.strip()}")

    avg_ms = (elapsed * 1000.0) / max(1, iterations)
    qps = iterations / elapsed
    return {
        "driver": "psql_session",
        "iterations": iterations,
        "elapsed_sec": round(elapsed, 6),
        "qps": round(qps, 2),
        "latency_avg_ms": round(avg_ms, 4),
        "latency_min_ms": round(avg_ms, 4),
        "latency_p50_ms": round(avg_ms, 4),
        "latency_p90_ms": round(avg_ms, 4),
        "latency_p95_ms": round(avg_ms, 4),
        "latency_p99_ms": round(avg_ms, 4),
        "latency_max_ms": round(avg_ms, 4),
        "latency_avg_us": round(avg_ms * 1000.0, 2),
        "latency_p50_us": round(avg_ms * 1000.0, 2),
        "latency_p95_us": round(avg_ms * 1000.0, 2),
    }


def calibrate_local(
    pgbench_bin: str,
    psql_bin: str,
    host: str,
    port: str,
    user: str,
    dbname: str,
    select1_iters: int,
    repeat1mb_iters: int,
) -> dict:
    use_pgbench = bool(pgbench_bin) and (
        os.path.exists(pgbench_bin) or subprocess.run(["which", pgbench_bin], capture_output=True).returncode == 0
    )

    if use_pgbench:
        s1 = run_pgbench_probe(pgbench_bin, host, port, user, dbname, SELECT_1_SQL, select1_iters)
        r1mb = run_pgbench_probe(pgbench_bin, host, port, user, dbname, REPEAT_1MB_SQL, repeat1mb_iters)
    else:
        s1 = run_psql_fallback_probe(psql_bin, host, port, user, dbname, SELECT_1_SQL, select1_iters)
        r1mb = run_psql_fallback_probe(psql_bin, host, port, user, dbname, REPEAT_1MB_SQL, repeat1mb_iters)

    s1["query"] = "SELECT 1;"

    total_bytes = int(r1mb["iterations"]) * PAYLOAD_BYTES
    elapsed_sec = max(1e-6, float(r1mb["elapsed_sec"]))
    rows_per_sec = float(r1mb["qps"])
    mb_per_sec = (total_bytes / 1_000_000.0) / elapsed_sec
    mib_per_sec = (total_bytes / (1024.0 * 1024.0)) / elapsed_sec
    gbps = (total_bytes * 8.0 / 1_000_000_000.0) / elapsed_sec

    r1mb_enriched = {
        "query": "SELECT repeat('0123456789', 100000);",
        "expression_equivalent": "repeat(100_000 * '0123456789')",
        "payload_bytes_per_row": PAYLOAD_BYTES,
        "iterations": r1mb["iterations"],
        "total_bytes_transferred": total_bytes,
        "elapsed_sec": r1mb["elapsed_sec"],
        "rows_per_sec": round(rows_per_sec, 2),
        "throughput_mb_per_sec": round(mb_per_sec, 2),
        "throughput_mib_per_sec": round(mib_per_sec, 2),
        "throughput_gbps": round(gbps, 4),
        "latency_avg_ms": r1mb["latency_avg_ms"],
        "latency_min_ms": r1mb["latency_min_ms"],
        "latency_p50_ms": r1mb["latency_p50_ms"],
        "latency_p90_ms": r1mb["latency_p90_ms"],
        "latency_p95_ms": r1mb["latency_p95_ms"],
        "latency_p99_ms": r1mb["latency_p99_ms"],
        "latency_max_ms": r1mb["latency_max_ms"],
        "driver": r1mb["driver"],
    }

    return {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "client_hostname": platform.node(),
        "server_endpoint": f"{host or 'localhost'}:{port or '5432'}/{dbname}",
        "select_1": s1,
        "select_repeat_1mb": r1mb_enriched,
    }


def calibrate_from_remote_client(
    client_ssh_target: str,
    client_ssh_opts: str,
    pgbench_bin: str,
    psql_bin: str,
    host: str,
    port: str,
    user: str,
    dbname: str,
    select1_iters: int,
    repeat1mb_iters: int,
) -> dict:
    script_source = Path(__file__).resolve().read_text(encoding="utf-8")
    remote_args = [
        "python3",
        "-",
        "--pgbench-bin",
        pgbench_bin,
        "--psql-bin",
        psql_bin,
        "--host",
        host,
        "--port",
        str(port),
        "--dbname",
        dbname,
        "--select1-iters",
        str(select1_iters),
        "--repeat1mb-iters",
        str(repeat1mb_iters),
    ]
    if user:
        remote_args.extend(["--user", user])

    ssh_cmd = ["ssh"]
    if client_ssh_opts:
        ssh_cmd.extend(shlex.split(client_ssh_opts))
    ssh_cmd.append(client_ssh_target)
    ssh_cmd.append(" ".join(shlex.quote(a) for a in remote_args))

    res = subprocess.run(
        ssh_cmd,
        input=script_source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=120,
        check=False,
    )
    if res.returncode != 0:
        raise RuntimeError(
            f"Remote calibration from {client_ssh_target} failed (exit {res.returncode}): {res.stderr.strip()}"
        )
    payload = json.loads(res.stdout)
    payload["client_ssh_target"] = client_ssh_target
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure client-server baseline latency (SELECT 1) and 1 MB payload throughput (SELECT repeat('0123456789', 100000))."
    )
    parser.add_argument("--pgbench-bin", default="pgbench", help="Path to pgbench binary")
    parser.add_argument("--psql-bin", default="psql", help="Path to psql binary")
    parser.add_argument("--host", default=os.environ.get("SERVER_HOST", "127.0.0.1"), help="PostgreSQL server host")
    parser.add_argument("--port", default=os.environ.get("PGPORT", "5432"), help="PostgreSQL server port")
    parser.add_argument("--user", default=os.environ.get("PGUSER", ""), help="PostgreSQL user")
    parser.add_argument("--dbname", default="postgres", help="PostgreSQL database name")
    parser.add_argument(
        "--select1-iters",
        type=int,
        default=int(os.environ.get("CALIBRATION_SELECT1_ITERS", "200")),
        help="Number of SELECT 1 iterations (default: 200)",
    )
    parser.add_argument(
        "--repeat1mb-iters",
        type=int,
        default=int(os.environ.get("CALIBRATION_REPEAT1MB_ITERS", "50")),
        help="Number of SELECT repeat('0123456789', 100000) iterations (default: 50)",
    )
    parser.add_argument(
        "--client-ssh-target",
        default="",
        help="Optional SSH target (user@host) when running calibration from a remote client VM",
    )
    parser.add_argument(
        "--client-ssh-opts",
        default="-o BatchMode=yes -o StrictHostKeyChecking=accept-new",
        help="SSH options for --client-ssh-target",
    )
    parser.add_argument("--output", default="", help="Optional path to write JSON calibration output")
    args = parser.parse_args()

    if args.client_ssh_target:
        payload = calibrate_from_remote_client(
            client_ssh_target=args.client_ssh_target,
            client_ssh_opts=args.client_ssh_opts,
            pgbench_bin=args.pgbench_bin,
            psql_bin=args.psql_bin,
            host=args.host,
            port=args.port,
            user=args.user,
            dbname=args.dbname,
            select1_iters=args.select1_iters,
            repeat1mb_iters=args.repeat1mb_iters,
        )
    else:
        payload = calibrate_local(
            pgbench_bin=args.pgbench_bin,
            psql_bin=args.psql_bin,
            host=args.host,
            port=args.port,
            user=args.user,
            dbname=args.dbname,
            select1_iters=args.select1_iters,
            repeat1mb_iters=args.repeat1mb_iters,
        )

    out_text = json.dumps(payload, indent=2)
    if args.output:
        out_path = Path(args.output)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(out_text + "\n", encoding="utf-8")
    else:
        print(out_text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
