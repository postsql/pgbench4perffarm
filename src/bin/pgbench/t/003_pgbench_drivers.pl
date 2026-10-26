# Copyright (c) 2026, PostgreSQL Global Development Group

#
# pgbench multi-database driver vtable tests (Quack and Cloud Spanner drivers)
#

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

my $python = `command -v python3 2>/dev/null`;
chomp($python);
if (!$python)
{
	plan skip_all => 'python3 not available for mock Quack/Spanner HTTP server tests';
}

my $testdir = "$PostgreSQL::Test::Utils::tmp_check/t_003_drivers";
mkdir $testdir unless -d $testdir;

my $mock_script = "$testdir/run_driver_mocks.py";
open my $fh, '>', $mock_script or BAIL_OUT("could not create $mock_script: $!");
print $fh <<'PYEOF';
import http.server
import json
import os
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

PGBENCH_BIN = shutil.which("pgbench") or "./pgbench"

def encode_quak_chunk(cols):
    nrows = len(cols[0][2]) if cols else 0
    ncols = len(cols)
    buf = bytearray(b"QUAK")
    buf += struct.pack("<BII", 1, ncols, nrows)
    for name, qtype, _ in cols:
        nb = name.encode("utf-8")
        buf += struct.pack("<H", len(nb)) + nb + struct.pack("<B", qtype)
    for _, qtype, values in cols:
        nbytes_valid = (nrows + 7) // 8
        valid = bytearray(nbytes_valid)
        for r, v in enumerate(values):
            if v is not None:
                valid[r // 8] |= (1 << (r % 8))
        buf += valid
        for v in values:
            if qtype == 1:
                buf += struct.pack("<B", 0 if not v else 1)
            elif qtype == 2:
                buf += struct.pack("<i", 0 if v is None else int(v))
            elif qtype == 3:
                buf += struct.pack("<q", 0 if v is None else int(v))
            elif qtype == 4:
                buf += struct.pack("<d", 0.0 if v is None else float(v))
            elif qtype == 5:
                s = b"" if v is None else str(v).encode("utf-8")
                buf += struct.pack("<I", len(s)) + s
    return bytes(buf)

class QuackState:
    def __init__(self):
        self.lock = threading.Lock()
        self.queries = []
        self.aborted_once = False
        self.response_format = "quak"

class QuackHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):
        pass
    def _send(self, status, ctype, body, extra=None, chunked=False):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Connection", "keep-alive")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            step = 65536
            for i in range(0, len(body), step):
                part = body[i:i+step]
                self.wfile.write(f"{len(part):x}\r\n".encode("ascii") + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
    def do_POST(self):
        clen = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(clen) if clen > 0 else b""
        st = self.server.quack_state
        sql = body.decode("utf-8").strip()
        with st.lock:
            st.queries.append(sql)
            fmt = st.response_format
        upper = sql.upper()
        if "SELECT ERROR_TEST" in upper:
            err = json.dumps({"error": "simulated error", "sqlstate": "23505"}).encode("utf-8")
            self._send(400, "application/json", err, {"X-DuckDB-SQLState": "23505"})
            return
        if "SELECT LARGE_PAYLOAD_TEST" in upper:
            big_str = "X" * (1100 * 1024)
            cols = [("big", 5, [big_str])]
            self._send(200, "application/duckdb", encode_quak_chunk(cols), chunked=True)
            return
        if "SELECT SERIALIZATION_TEST" in upper:
            with st.lock:
                if not st.aborted_once:
                    st.aborted_once = True
                    err = json.dumps({"error": "conflict", "sqlstate": "40001"}).encode("utf-8")
                    self._send(409, "application/json", err, {"X-DuckDB-SQLState": "40001"})
                    return
        if "SELECT COUNT(*) FROM PGBENCH_BRANCHES" in upper:
            cols = [("count", 3, [1])]
        elif "SELECT ABALANCE FROM PGBENCH_ACCOUNTS" in upper:
            cols = [("abalance", 3, [4200])]
        elif "SELECT " in upper and "AS X" in upper:
            cols = [("x", 3, [55]), ("label", 5, ["quack_ok"])]
        elif "SELECT" in upper:
            cols = [("val", 3, [1])]
        else:
            if fmt == "quak":
                self._send(200, "application/duckdb", b"QUAK" + struct.pack("<BII", 0, 0, 1))
            else:
                self._send(200, "application/json", b'{"status":"ok","rows_affected":1}')
            return
        if fmt == "quak":
            self._send(200, "application/duckdb", encode_quak_chunk(cols))
        elif fmt == "json":
            jcols = [c[0] for c in cols]
            nrows = len(cols[0][2])
            jrows = [[c[2][r] for c in cols] for r in range(nrows)]
            self._send(200, "application/json", json.dumps({"columns": jcols, "rows": jrows}).encode("utf-8"))
        else:
            header = "\t".join(c[0] for c in cols) + "\n"
            nrows = len(cols[0][2])
            lines = [header] + ["\t".join(str(c[2][r]) for c in cols) + "\n" for r in range(nrows)]
            self._send(200, "text/tab-separated-values", "".join(lines).encode("utf-8"))

class SpannerState:
    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = 0
        self.deleted_sessions = 0
        self.op_polls = 0
        self.txns = 0
        self.ddl_statements = []
        self.sql_calls = []
        self.batch_dml_calls = []
        self.commits = []
        self.rollbacks = 0
        self.fail_next_commit_aborted = False
        self.fail_next_batch_dml_aborted = False

class SpannerHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):
        pass
    def _send_json(self, status, obj, chunked=False):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "keep-alive")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            mid = max(1, len(body) // 2)
            for part in (body[:mid], body[mid:]):
                if part:
                    self.wfile.write(f"{len(part):x}\r\n".encode("ascii") + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
    def do_GET(self):
        if "/operations/" in self.path:
            with self.server.spanner_state.lock:
                self.server.spanner_state.op_polls += 1
            self._send_json(200, {"name": self.path.lstrip("/v1/"), "done": True})
        else:
            self._send_json(404, {"error": {"code": 404, "message": "not found"}})
    def do_PATCH(self):
        clen = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(clen).decode("utf-8")) if clen > 0 else {}
        stmts = req.get("statements", [])
        for s in stmts:
            if s.strip().endswith(";"):
                self._send_json(400, {"error": {"code": 400, "message": "DDL trailing semicolon", "status": "INVALID_ARGUMENT"}})
                return
        with self.server.spanner_state.lock:
            self.server.spanner_state.ddl_statements.extend(stmts)
        self._send_json(200, {"name": "projects/p/instances/i/databases/d/operations/ddl-1", "done": False})
    def do_DELETE(self):
        clen = int(self.headers.get("Content-Length", "0"))
        if clen > 0:
            self.rfile.read(clen)
        with self.server.spanner_state.lock:
            self.server.spanner_state.deleted_sessions += 1
        self._send_json(200, {})
    def do_POST(self):
        clen = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(clen).decode("utf-8")) if clen > 0 else {}
        st = self.server.spanner_state
        if self.path.endswith("/sessions"):
            with st.lock:
                st.sessions += 1
                sname = f"projects/p/instances/i/databases/d/sessions/s{st.sessions}"
            self._send_json(200, {"name": sname})
        elif self.path.endswith(":beginTransaction"):
            with st.lock:
                st.txns += 1
                tid = f"tx-{st.txns}"
            self._send_json(200, {
                "id": tid,
                "precommitToken": {"precommitToken": f"tok-begin-{tid}", "seqNum": 1}
            })
        elif self.path.endswith(":executeSql"):
            sql = req.get("sql", "").strip()
            params = req.get("params", {})
            tx_sel = req.get("transaction", {})
            with st.lock:
                st.sql_calls.append((sql, params))
                new_tid = None
                if "begin" in tx_sel:
                    st.txns += 1
                    new_tid = f"tx-inline-{st.txns}"
            upper = sql.upper()
            meta = {"rowType": {"fields": []}}
            if new_tid:
                meta["transaction"] = {"id": new_tid}
            if "SELECT COUNT(*) FROM PGBENCH_BRANCHES" in upper:
                meta["rowType"]["fields"] = [{"name": "count", "type": {"code": "INT64"}}]
                self._send_json(200, {"metadata": meta, "rows": [["1"]]}, chunked=True)
            elif "SELECT ABALANCE FROM PGBENCH_ACCOUNTS" in upper:
                meta["rowType"]["fields"] = [{"name": "abalance", "type": {"code": "INT64"}}]
                self._send_json(200, {"metadata": meta, "rows": [["777"]]})
            elif "SELECT" in upper:
                p1 = params.get("p1", "10")
                meta["rowType"]["fields"] = [
                    {"name": "echo_val", "type": {"code": "INT64"}},
                    {"name": "dialect_tag", "type": {"code": "STRING"}}
                ]
                self._send_json(200, {"metadata": meta, "rows": [[str(p1), "spanner_ok"]]}, chunked=True)
            else:
                self._send_json(200, {
                    "metadata": meta,
                    "stats": {"rowCountExact": "1"},
                    "precommitToken": {"precommitToken": "tok-dml", "seqNum": 2}
                })
        elif self.path.endswith(":executeBatchDml"):
            stmts = req.get("statements", [])
            tx_sel = req.get("transaction", {})
            with st.lock:
                if st.fail_next_batch_dml_aborted:
                    st.fail_next_batch_dml_aborted = False
                    self._send_json(200, {
                        "resultSets": [],
                        "status": {"code": 10, "message": "BatchDml aborted"}
                    })
                    return
                st.batch_dml_calls.append(stmts)
                new_tid = None
                if "begin" in tx_sel:
                    st.txns += 1
                    new_tid = f"tx-batch-{st.txns}"
            rsets = []
            for idx in range(len(stmts)):
                rs = {"stats": {"rowCountExact": "1"}}
                if idx == 0 and new_tid:
                    rs["metadata"] = {"transaction": {"id": new_tid}}
                rsets.append(rs)
            self._send_json(200, {
                "resultSets": rsets,
                "status": {"code": 0},
                "precommitToken": {"precommitToken": "tok-batch-dml", "seqNum": 3}
            })
        elif self.path.endswith(":commit"):
            with st.lock:
                if st.fail_next_commit_aborted:
                    st.fail_next_commit_aborted = False
                    self._send_json(409, {"error": {"code": 409, "message": "aborted", "status": "ABORTED"}})
                    return
                st.commits.append(req)
            self._send_json(200, {"commitTimestamp": "2026-10-26T18:00:00Z"})
        elif self.path.endswith(":rollback"):
            with st.lock:
                st.rollbacks += 1
            self._send_json(200, {})
        else:
            self._send_json(404, {"error": {"code": 404, "message": "not found", "status": "NOT_FOUND"}})

class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

def run_cmd(args):
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"Failed ({proc.returncode}): {' '.join(args)}\n{proc.stdout}\n{proc.stderr}")
    return proc

def main():
    # Quack tests
    qsrv = ThreadedHTTPServer(("127.0.0.1", 0), QuackHandler)
    qsrv.quack_state = QuackState()
    qport = qsrv.server_address[1]
    threading.Thread(target=qsrv.serve_forever, daemon=True).start()
    try:
        quri = f"quack://127.0.0.1:{qport}/benchdb"
        run_cmd([PGBENCH_BIN, "-i", "-s", "1", quri])
        for mode, fmt in [("simple", "quak"), ("extended", "quak"), ("prepared", "json"), ("extended", "tsv")]:
            qsrv.quack_state.response_format = fmt
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "3", "-c", "2", "-j", "2", "-M", mode, quri])
            assert "number of transactions actually processed: 6/6" in r.stdout

        # Test SQL comment with apostrophe, large (>1MB) chunked payload, and high concurrency (-c 16 -j 4)
        qsrv.quack_state.response_format = "json"
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
            f.write(
                "\\set x 55\n"
                "/* client's comment with apostrophe */ SELECT :x AS x, 'quack_ok' AS label \\gset\n"
                "SELECT large_payload_test;\n"
                "UPDATE pgbench_branches SET bbalance = :x \\gset_meta m_\n"
                "\\if :m_nrows != 1\n"
                "  SELECT error_test;\n"
                "\\endif\n"
            )
            qpath = f.name
        try:
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "2", "-c", "8", "-j", "4", "-M", "extended", "-f", qpath, quri])
            assert "number of transactions actually processed: 16/16" in r.stdout
        finally:
            os.unlink(qpath)
    finally:
        qsrv.shutdown()
        qsrv.server_close()

    # Spanner tests
    ssrv = ThreadedHTTPServer(("127.0.0.1", 0), SpannerHandler)
    ssrv.spanner_state = SpannerState()
    sport = ssrv.server_address[1]
    threading.Thread(target=ssrv.serve_forever, daemon=True).start()
    try:
        suri = f"spanner://projects/p/instances/i/databases/d?endpoint=127.0.0.1:{sport}"
        run_cmd([PGBENCH_BIN, "-i", "-s", "1", suri])
        assert any("INT64" in ddl for ddl in ssrv.spanner_state.ddl_statements)
        assert ssrv.spanner_state.op_polls >= len(ssrv.spanner_state.ddl_statements)

        # Default mode on Spanner should automatically use prefer_prepared_mode (@p1) without -M flag
        ssrv.spanner_state.sql_calls.clear()
        r = run_cmd([PGBENCH_BIN, "-n", "-t", "3", "-c", "4", "-j", "2", "--dialect=googlesql", suri])
        assert "number of transactions actually processed: 12/12" in r.stdout
        assert any("@p1" in c[0] for c in ssrv.spanner_state.sql_calls)

        ssrv.spanner_state.sql_calls.clear()
        r = run_cmd([PGBENCH_BIN, "-n", "-t", "3", "-c", "2", "-j", "2", "-M", "extended", "--dialect=postgresql", suri])
        assert "number of transactions actually processed: 6/6" in r.stdout
        assert any("$1" in c[0] for c in ssrv.spanner_state.sql_calls)

        ssrv.spanner_state.fail_next_commit_aborted = True
        ssrv.spanner_state.fail_next_batch_dml_aborted = True
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
            f.write(
                "\\set delta 15\n"
                "/* leading comment */ SELECT :delta AS echo_val, 'spanner_ok' AS dialect_tag \\gset\n"
                "BEGIN;\n"
                "\\startpipeline\n"
                "/* dml comment */ UPDATE pgbench_branches SET bbalance = bbalance + :echo_val WHERE bid = 1;\n"
                "UPDATE pgbench_tellers SET tbalance = tbalance + :echo_val WHERE tid = 1;\n"
                "\\endpipeline\n"
                "UPDATE pgbench_accounts SET abalance = abalance + :echo_val WHERE aid = 1 \\gset_meta bm_\n"
                "\\if :bm_nrows != 1\n"
                "  SELECT invalid_batch_nrows;\n"
                "\\endif\n"
                "COMMIT;\n"
            )
            spath = f.name
        try:
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "2", "-c", "1", "--max-tries", "4", "-f", spath, suri])
            assert "number of transactions actually processed: 2/2" in r.stdout
            assert "number of transactions retried: 1" in r.stdout
            assert "total number of retries: 2" in r.stdout
            assert len(ssrv.spanner_state.batch_dml_calls) >= 2
            assert ssrv.spanner_state.rollbacks >= 1
            assert any(c.get("precommitToken", {}).get("precommitToken") == "tok-batch-dml" for c in ssrv.spanner_state.commits)
        finally:
            os.unlink(spath)

        # Verify all created Spanner sessions were cleanly deleted on connection finish
        assert ssrv.spanner_state.deleted_sessions == ssrv.spanner_state.sessions
    finally:
        ssrv.shutdown()
        ssrv.server_close()

if __name__ == "__main__":
    main()
PYEOF
close $fh;

command_ok([ $python, $mock_script ], 'quack and spanner drivers end-to-end protocol tests');

done_testing();
