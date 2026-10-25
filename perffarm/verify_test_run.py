#!/usr/bin/env python3
"""
verify_test_run.py — 5-point test result verifier, bundle builder, and
Metrics DB (`perffarm_test_runs`) recorder for pgbench4perffarm.

5-Point Verification Contract:
  1. pgbench exit code == 0
  2. transactions > 0 and unexpected failed_transactions == 0
  3. 100% \validate_result pass rate (validation_failed == 0)
  4. --sla compliance (sla_all_passed == true)
  5. Zero PANIC / TRAP / FailedAssertion / unexpected FATAL errors in PostgreSQL server log
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys


BENIGN_FATAL_PATTERNS = [
    r"the database system is starting up",
    r"the database system is shutting down",
    r"terminating .* due to administrator command",
    r"canceling authentication due to timeout",
]


def inspect_server_log(log_path: str) -> dict:
    if not log_path or not Path(log_path).exists():
        return {
            "log_exists": False,
            "severe_errors_count": 0,
            "severe_error_lines": [],
            "clean": True,
        }

    severe_lines = []
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if any(tok in line for tok in ("PANIC:", "TRAP:", "FailedAssertion", "Segmentation fault", "FATAL:")):
                    if "FATAL:" in line and any(re.search(pat, line) for pat in BENIGN_FATAL_PATTERNS):
                        continue
                    severe_lines.append(line.strip()[:300])
    except Exception as exc:
        severe_lines.append(f"Log read error: {exc}")

    return {
        "log_exists": True,
        "severe_errors_count": len(severe_lines),
        "severe_error_lines": severe_lines[:20],
        "clean": len(severe_lines) == 0,
    }


def load_json_if_exists(path_str: str) -> dict:
    if not path_str:
        return {}
    p = Path(path_str)
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {}


def sql_literal(val) -> str:
    if val is None:
        return "NULL"
    if isinstance(val, bool):
        return "TRUE" if val else "FALSE"
    if isinstance(val, (int, float)):
        return str(val)
    s = str(val).replace("'", "''")
    return f"'{s}'"


def sql_jsonb(obj) -> str:
    if obj is None:
        return "'{}'::jsonb"
    s = json.dumps(obj).replace("'", "''")
    return f"'{s}'::jsonb"


def record_to_metrics_db(
    psql_bin: str,
    conninfo: str,
    schema: str,
    row_data: dict,
) -> int | None:
    run_label = row_data.get("run_label")
    pgbench_run_id_expr = "NULL"
    if row_data.get("pgbench_run_id"):
        pgbench_run_id_expr = str(int(row_data["pgbench_run_id"]))
    elif run_label:
        pgbench_run_id_expr = (
            f"(SELECT max(run_id) FROM {schema}.pgbench_runs WHERE run_label = {sql_literal(run_label)})"
        )

    sql = f"""
SET search_path TO {schema}, public, pg_catalog;
INSERT INTO {schema}.perffarm_test_runs (
    batch_id,
    test_name,
    suite_name,
    pgbench_run_id,
    scale,
    clients,
    threads,
    duration_sec,
    init_duration_sec,
    template_reused,
    db_size_bytes,
    server_settings,
    baseline_calibration,
    pg_stat_database_delta,
    pg_stat_bgwriter_delta,
    pg_stat_checkpointer_delta,
    pg_stat_wal_delta,
    pg_stat_io_delta,
    pg_stat_statements_top,
    os_metrics,
    client_os_metrics,
    verification_status,
    verification_details,
    started_at,
    ended_at,
    uploaded_at
) VALUES (
    {int(row_data['batch_id'])},
    {sql_literal(row_data['test_name'])},
    {sql_literal(row_data['suite_name'])},
    {pgbench_run_id_expr},
    {sql_literal(row_data.get('scale'))},
    {sql_literal(row_data.get('clients'))},
    {sql_literal(row_data.get('threads'))},
    {sql_literal(row_data.get('duration_sec'))},
    {sql_literal(row_data.get('init_duration_sec'))},
    {sql_literal(bool(row_data.get('template_reused', False)))},
    {sql_literal(row_data.get('db_size_bytes'))},
    {sql_jsonb(row_data.get('server_settings', {}))},
    {sql_jsonb(row_data.get('baseline_calibration', {}))},
    {sql_jsonb(row_data.get('pg_stat_database_delta', {}))},
    {sql_jsonb(row_data.get('pg_stat_bgwriter_delta', {}))},
    {sql_jsonb(row_data.get('pg_stat_checkpointer_delta', {}))},
    {sql_jsonb(row_data.get('pg_stat_wal_delta', {}))},
    {sql_jsonb(row_data.get('pg_stat_io_delta', []))},
    {sql_jsonb(row_data.get('pg_stat_statements_top', []))},
    {sql_jsonb(row_data.get('os_metrics', {}))},
    {sql_jsonb(row_data.get('client_os_metrics', {}))},
    {sql_literal(row_data['verification_status'])},
    {sql_jsonb(row_data.get('verification_details', {}))},
    COALESCE({sql_literal(row_data.get('started_at'))}::timestamptz, clock_timestamp()),
    COALESCE({sql_literal(row_data.get('ended_at'))}::timestamptz, clock_timestamp()),
    clock_timestamp()
) RETURNING test_run_id;
"""
    cmd = [psql_bin, "-X", "-q", "-t", "-A", "-d", conninfo, "-c", sql]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if res.returncode != 0:
        sys.stderr.write(f"[verify_test_run] Warning: Failed to insert perffarm_test_runs: {res.stderr.strip()}\n")
        return None
    for line in reversed(res.stdout.strip().splitlines()):
        if line.strip().isdigit():
            return int(line.strip())
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify pgbench test run and record in Metrics DB.")
    parser.add_argument("--batch-id", type=int, required=True)
    parser.add_argument("--test-name", required=True)
    parser.add_argument("--suite-name", required=True)
    parser.add_argument("--run-label", default="")
    parser.add_argument("--status-override", choices=["", "SKIPPED", "FAILED"], default="")
    parser.add_argument("--skip-reason", default="")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--init-duration-sec", type=float, default=0.0)
    parser.add_argument("--template-reused", action="store_true")
    parser.add_argument("--started-at", default="")
    parser.add_argument("--ended-at", default="")
    parser.add_argument("--json-report", default="")
    parser.add_argument("--pg-stats-delta", default="")
    parser.add_argument("--baseline-calibration", default="")
    parser.add_argument("--server-info", default="")
    parser.add_argument("--client-info", default="")
    parser.add_argument("--server-log", default="")
    parser.add_argument("--bundle-output", required=True)
    parser.add_argument("--psql-bin", default="psql")
    parser.add_argument("--metrics-db-conninfo", default="")
    parser.add_argument("--metrics-db-schema", default="public")
    args = parser.parse_args()

    report = load_json_if_exists(args.json_report)
    stats_delta = load_json_if_exists(args.pg_stats_delta)
    calibration = load_json_if_exists(args.baseline_calibration)
    server_info = load_json_if_exists(args.server_info)
    client_info = load_json_if_exists(args.client_info)
    log_check = inspect_server_log(args.server_log)

    if args.status_override == "SKIPPED":
        verification_status = "SKIPPED"
        verification_details = {
            "status": "SKIPPED",
            "reason": args.skip_reason or "Unsupported optional SQL feature or extension",
            "checks": {},
        }
        duration_sec = 0.0
    else:
        overall = report.get("overall", {})
        tx_cnt = int(overall.get("transactions_processed") or overall.get("transactions") or 0)
        tx_failed = int(overall.get("transactions_failed") or overall.get("failed_transactions") or 0)
        val_passed = int(overall.get("validation_passed") or 0)
        val_failed = int(overall.get("validation_failed") or 0)
        sla_all_passed = bool(report.get("sla_all_passed", True))
        duration_sec = float(overall.get("duration_sec") or report.get("duration_sec") or 0.0)

        chk_exit = args.exit_code == 0 and bool(report)
        chk_tx = tx_cnt > 0 and tx_failed == 0
        chk_val = val_failed == 0
        chk_sla = sla_all_passed
        chk_log = bool(log_check.get("clean", True))

        all_ok = chk_exit and chk_tx and chk_val and chk_sla and chk_log and args.status_override != "FAILED"
        verification_status = "PASSED" if all_ok else "FAILED"

        failure_reasons = []
        if not chk_exit:
            failure_reasons.append(f"pgbench exit_code={args.exit_code} (report_present={bool(report)})")
        if not chk_tx:
            failure_reasons.append(f"transactions={tx_cnt}, failed_transactions={tx_failed}")
        if not chk_val:
            failure_reasons.append(f"validation_failed={val_failed} (passed={val_passed})")
        if not chk_sla:
            failure_reasons.append("One or more --sla rules failed")
        if not chk_log:
            failure_reasons.append(f"Server log had {log_check['severe_errors_count']} PANIC/FATAL/TRAP lines")
        if args.skip_reason and not all_ok:
            failure_reasons.append(args.skip_reason)

        verification_details = {
            "status": verification_status,
            "failure_reasons": failure_reasons,
            "checks": {
                "1_exit_code_zero": {
                    "passed": chk_exit,
                    "exit_code": args.exit_code,
                    "report_present": bool(report),
                },
                "2_transactions_healthy": {
                    "passed": chk_tx,
                    "transactions": tx_cnt,
                    "failed_transactions": tx_failed,
                    "expected_rollbacks": int(overall.get("expected_rollbacks") or 0),
                    "tps": float(overall.get("tps") or 0.0),
                },
                "3_result_validation_100pct": {
                    "passed": chk_val,
                    "validation_passed": val_passed,
                    "validation_failed": val_failed,
                },
                "4_sla_compliance": {
                    "passed": chk_sla,
                    "sla_all_passed": sla_all_passed,
                    "sla_verifications": report.get("sla_verifications", []),
                },
                "5_server_log_clean": log_check,
            },
        }

    cfg = report.get("configuration", {})
    row_data = {
        "batch_id": args.batch_id,
        "test_name": args.test_name,
        "suite_name": args.suite_name,
        "run_label": args.run_label,
        "pgbench_run_id": report.get("run_id"),
        "scale": int(cfg.get("scale") or report.get("scale") or args.scale),
        "clients": int(cfg.get("clients") or report.get("clients") or args.clients),
        "threads": int(cfg.get("threads") or report.get("threads") or args.threads),
        "duration_sec": round(duration_sec, 6),
        "init_duration_sec": round(args.init_duration_sec, 4),
        "template_reused": args.template_reused,
        "db_size_bytes": stats_delta.get("db_size_bytes"),
        "server_settings": stats_delta.get("server_settings", {}),
        "baseline_calibration": calibration,
        "pg_stat_database_delta": stats_delta.get("pg_stat_database_delta", {}),
        "pg_stat_bgwriter_delta": stats_delta.get("pg_stat_bgwriter_delta", {}),
        "pg_stat_checkpointer_delta": stats_delta.get("pg_stat_checkpointer_delta", {}),
        "pg_stat_wal_delta": stats_delta.get("pg_stat_wal_delta", {}),
        "pg_stat_io_delta": stats_delta.get("pg_stat_io_delta", []),
        "pg_stat_statements_top": stats_delta.get("pg_stat_statements_top", []),
        "os_metrics": stats_delta.get("os_metrics", {}),
        "client_os_metrics": stats_delta.get("client_os_metrics", {}),
        "verification_status": verification_status,
        "verification_details": verification_details,
        "started_at": args.started_at or datetime.now(timezone.utc).isoformat(),
        "ended_at": args.ended_at or datetime.now(timezone.utc).isoformat(),
    }

    test_run_id = None
    if args.metrics_db_conninfo:
        test_run_id = record_to_metrics_db(
            psql_bin=args.psql_bin,
            conninfo=args.metrics_db_conninfo,
            schema=args.metrics_db_schema,
            row_data=row_data,
        )

    bundle = {
        "test_run_id": test_run_id,
        **row_data,
        "server_instance_info": server_info,
        "client_instance_info": client_info,
        "pgbench_report": report,
    }

    out_path = Path(args.bundle_output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")

    print(
        json.dumps(
            {
                "test_run_id": test_run_id,
                "test_name": args.test_name,
                "verification_status": verification_status,
                "tps": report.get("overall", {}).get("tps"),
                "failure_reasons": verification_details.get("failure_reasons", []),
            }
        )
    )

    return 0 if verification_status in ("PASSED", "SKIPPED") else 1


if __name__ == "__main__":
    sys.exit(main())
