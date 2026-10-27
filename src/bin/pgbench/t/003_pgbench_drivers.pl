# Copyright (c) 2026, PostgreSQL Global Development Group

#
# pgbench multi-database driver vtable tests (Quack and Cloud Spanner drivers)
#

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
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

    # MySQL wire protocol v10 tests
    run_mysql_tests()

    # Neo4j HTTP Transaction Cypher API tests
    run_neo4j_tests()

def mysql_pkt(seq_id, payload):
    length = len(payload)
    return bytes([length & 0xFF, (length >> 8) & 0xFF, (length >> 16) & 0xFF, seq_id & 0xFF]) + payload

def mysql_lenenc_int(val):
    if val < 251:
        return bytes([val])
    elif val < 65536:
        return b"\xfc" + struct.pack("<H", val)
    elif val < 16777216:
        return b"\xfd" + bytes([val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF])
    else:
        return b"\xfe" + struct.pack("<Q", val)

def mysql_lenenc_str(s):
    if s is None:
        return b"\xfb"
    b = s.encode("utf-8") if isinstance(s, str) else bytes(s)
    return mysql_lenenc_int(len(b)) + b

def mysql_col_def_pkt(seq_id, name, col_type=0xFD, flags=0):
    body = (
        mysql_lenenc_str("def") +
        mysql_lenenc_str("benchdb") +
        mysql_lenenc_str("tbl") +
        mysql_lenenc_str("tbl") +
        mysql_lenenc_str(name) +
        mysql_lenenc_str(name) +
        b"\x0c" +
        struct.pack("<H", 33) +
        struct.pack("<I", 64) +
        bytes([col_type]) +
        struct.pack("<H", flags) +
        b"\x00\x00\x00"
    )
    return mysql_pkt(seq_id, body)

def mysql_ok_pkt(seq_id, affected_rows=1, status_flags=0):
    body = b"\x00" + mysql_lenenc_int(affected_rows) + mysql_lenenc_int(0) + struct.pack("<HH", status_flags, 0)
    return mysql_pkt(seq_id, body)

def mysql_eof_pkt(seq_id, status_flags=0):
    body = b"\xfe\x00\x00" + struct.pack("<H", status_flags)
    return mysql_pkt(seq_id, body)

def mysql_err_pkt(seq_id, err_code, sqlstate, msg):
    body = b"\xff" + struct.pack("<H", err_code) + b"#" + sqlstate.encode("ascii")[:5] + msg.encode("utf-8")
    return mysql_pkt(seq_id, body)

class MysqlState:
    def __init__(self):
        self.lock = threading.Lock()
        self.queries = []
        self.prepared_sqls = {}
        self.next_stmt_id = 1
        self.stmt_executions = 0
        self.deadlock_once = True
        self.caching_sha2_authed = 0
        self.multi_stmt_queries = 0

class MysqlConnHandler(socketserver.BaseRequestHandler):
    def _read_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def _read_pkt(self):
        hdr = self._read_exact(4)
        if not hdr:
            return None, None
        plen = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16)
        seq = hdr[3]
        payload = self._read_exact(plen) if plen > 0 else b""
        if payload is None:
            return None, None
        return seq, payload

    def handle(self):
        import hashlib
        st = self.server.mysql_state
        # Send Protocol v10 Initial Handshake Packet
        scramble1 = b"12345678"
        scramble2 = b"901234567890\x00"
        caps = 0x0008820F
        cap_low = caps & 0xFFFF
        cap_high = (caps >> 16) & 0xFFFF
        greeting = (
            b"\x0a" +
            b"8.0.36-mock\x00" +
            struct.pack("<I", 1) +
            scramble1 +
            b"\x00" +
            struct.pack("<H", cap_low) +
            b"\x21" +
            struct.pack("<H", 0x0002) +
            struct.pack("<H", cap_high) +
            bytes([21]) +
            (b"\x00" * 10) +
            scramble2 +
            b"mysql_native_password\x00"
        )
        self.request.sendall(mysql_pkt(0, greeting))

        seq, auth_resp = self._read_pkt()
        if auth_resp is None:
            return

        # Exercise AuthSwitchRequest (0xFE) to caching_sha2_password + AuthMoreData (0x01 0x03)
        switch_scramble = b"ABCDEFGHIJKLMNOPQRST"
        auth_switch = b"\xfe" + b"caching_sha2_password\x00" + switch_scramble + b"\x00"
        self.request.sendall(mysql_pkt(seq + 1, auth_switch))

        seq2, sha2_token = self._read_pkt()
        if sha2_token is None:
            return
        pw = b"secretpass"
        d1 = hashlib.sha256(pw).digest()
        d2 = hashlib.sha256(d1).digest()
        d3 = hashlib.sha256(d2 + switch_scramble).digest()
        expected_sha2 = bytes(a ^ b for a, b in zip(d1, d3))
        if sha2_token == expected_sha2:
            with st.lock:
                st.caching_sha2_authed += 1
            # Fast-auth success (0x01 0x03) followed by OK_Packet (0x00)
            self.request.sendall(
                mysql_pkt(seq2 + 1, b"\x01\x03") +
                mysql_ok_pkt(seq2 + 2, affected_rows=0)
            )
        else:
            self.request.sendall(mysql_err_pkt(seq2 + 1, 1045, "28000", "caching_sha2_password mismatch"))
            return

        in_tx = False
        while True:
            seq, payload = self._read_pkt()
            if payload is None or len(payload) == 0:
                break
            cmd = payload[0]
            if cmd == 0x01:  # COM_QUIT
                break
            elif cmd == 0x03:  # COM_QUERY
                sql = payload[1:].decode("utf-8", errors="replace").strip()
                with st.lock:
                    st.queries.append(sql)
                upper = sql.upper()
                if upper.startswith("BEGIN") or upper.startswith("START TRANSACTION"):
                    in_tx = True
                    self.request.sendall(mysql_ok_pkt(1, 0, 0x0001))
                elif upper.startswith("COMMIT") or upper == "END":
                    in_tx = False
                    self.request.sendall(mysql_ok_pkt(1, 0, 0x0000))
                elif upper.startswith("ROLLBACK"):
                    in_tx = False
                    self.request.sendall(mysql_ok_pkt(1, 0, 0x0000))
                elif "DEADLOCK_TEST" in upper:
                    with st.lock:
                        should_fail = st.deadlock_once
                        st.deadlock_once = False
                    if should_fail:
                        self.request.sendall(mysql_err_pkt(1, 1213, "40001", "Deadlock found when trying to get lock"))
                    else:
                        self.request.sendall(mysql_ok_pkt(1, 1, 0x0001 if in_tx else 0))
                elif "SELECT COUNT(*) FROM PGBENCH_BRANCHES" in upper:
                    sflags = 0x0001 if in_tx else 0
                    resp = (
                        mysql_pkt(1, mysql_lenenc_int(1)) +
                        mysql_col_def_pkt(2, "count") +
                        mysql_eof_pkt(3, sflags) +
                        mysql_pkt(4, mysql_lenenc_str("1")) +
                        mysql_eof_pkt(5, sflags)
                    )
                    self.request.sendall(resp)
                elif "SELECT ABALANCE FROM PGBENCH_ACCOUNTS" in upper:
                    sflags = 0x0001 if in_tx else 0
                    resp = (
                        mysql_pkt(1, mysql_lenenc_int(1)) +
                        mysql_col_def_pkt(2, "abalance") +
                        mysql_eof_pkt(3, sflags) +
                        mysql_pkt(4, mysql_lenenc_str("888")) +
                        mysql_eof_pkt(5, sflags)
                    )
                    self.request.sendall(resp)
                elif upper.startswith("SELECT"):
                    sflags = 0x0001 if in_tx else 0
                    resp = (
                        mysql_pkt(1, mysql_lenenc_int(2)) +
                        mysql_col_def_pkt(2, "val_out") +
                        mysql_col_def_pkt(3, "tag_out") +
                        mysql_eof_pkt(4, sflags) +
                        mysql_pkt(5, mysql_lenenc_str("42") + mysql_lenenc_str("mysql_ok")) +
                        mysql_eof_pkt(6, sflags)
                    )
                    self.request.sendall(resp)
                elif ";" in sql:
                    parts = [p.strip() for p in sql.split(";") if p.strip()]
                    with st.lock:
                        st.multi_stmt_queries += 1
                    base_flags = 0x0001 if in_tx else 0
                    out = bytearray()
                    for idx, _ in enumerate(parts):
                        more_flag = 0x0008 if idx + 1 < len(parts) else 0x0000
                        out.extend(mysql_ok_pkt(idx + 1, 1, base_flags | more_flag))
                    self.request.sendall(bytes(out))
                else:
                    sflags = 0x0001 if in_tx else 0
                    self.request.sendall(mysql_ok_pkt(1, 1, sflags))
            elif cmd == 0x16:  # COM_STMT_PREPARE
                sql = payload[1:].decode("utf-8", errors="replace").strip()
                upper = sql.upper()
                if upper in ("BEGIN", "COMMIT", "ROLLBACK"):
                    # Exercise error 1295 ER_UNSUPPORTED_PS client-emulation fallback
                    self.request.sendall(mysql_err_pkt(1, 1295, "HY000", "This command is not supported in the prepared statement protocol yet"))
                    continue
                num_params = sql.count("?")
                is_select = upper.startswith("SELECT")
                num_cols = 1 if is_select else 0
                with st.lock:
                    stmt_id = st.next_stmt_id
                    st.next_stmt_id += 1
                    st.prepared_sqls[stmt_id] = (sql, num_params, num_cols)
                prep_ok = b"\x00" + struct.pack("<IHHB", stmt_id, num_cols, num_params, 0) + struct.pack("<H", 0)
                out = bytearray(mysql_pkt(1, prep_ok))
                cur_seq = 2
                if num_params > 0:
                    for i in range(num_params):
                        out.extend(mysql_col_def_pkt(cur_seq, f"p{i+1}"))
                        cur_seq += 1
                    out.extend(mysql_eof_pkt(cur_seq))
                    cur_seq += 1
                if num_cols > 0:
                    out.extend(mysql_col_def_pkt(cur_seq, "abalance", col_type=0x08))
                    cur_seq += 1
                    out.extend(mysql_eof_pkt(cur_seq))
                self.request.sendall(bytes(out))
            elif cmd == 0x17:  # COM_STMT_EXECUTE
                stmt_id = struct.unpack("<I", payload[1:5])[0]
                with st.lock:
                    st.stmt_executions += 1
                    sql, num_params, num_cols = st.prepared_sqls.get(stmt_id, ("", 0, 0))
                sflags = 0x0001 if in_tx else 0
                if num_cols > 0:
                    # Send Binary Protocol Resultset (0x00 header + NULL bitmap + int64 999)
                    bin_row = b"\x00\x00" + struct.pack("<q", 999)
                    resp = (
                        mysql_pkt(1, mysql_lenenc_int(1)) +
                        mysql_col_def_pkt(2, "abalance", col_type=0x08) +
                        mysql_eof_pkt(3, sflags) +
                        mysql_pkt(4, bin_row) +
                        mysql_eof_pkt(5, sflags)
                    )
                    self.request.sendall(resp)
                else:
                    self.request.sendall(mysql_ok_pkt(1, 1, sflags))

class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

def create_canonical_test_dataset(base_dir):
    import hashlib
    csv_content = (
        "id,name,created_on,payload,raw_bytes\n"
        "101,alpha,2026-01-15,\"{\"\"k\"\":1}\",\\xdeadbeef\n"
        "102,beta,2026-02-20,\"{\"\"k\"\":2}\",\\x01020304\n"
    ).encode("utf-8")
    csv_sha = hashlib.sha256(csv_content).hexdigest()
    with open(os.path.join(base_dir, "custom_items.csv"), "wb") as f:
        f.write(csv_content)
    manifest = {
        "format_version": 1,
        "workload": "custom",
        "scale": 1,
        "seed": 42,
        "tables": [
            {
                "name": "custom_items",
                "file": "custom_items.csv",
                "rows": 2,
                "sha256": csv_sha,
                "columns": ["id", "name", "created_on", "payload", "raw_bytes"],
                "column_types": ["INT64", "STRING", "DATE", "JSON", "BYTES"]
            }
        ],
        "sequences": [],
        "reservoirs": []
    }
    with open(os.path.join(base_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f)

def run_mysql_tests():
    msrv = ThreadedTCPServer(("127.0.0.1", 0), MysqlConnHandler)
    msrv.mysql_state = MysqlState()
    mport = msrv.server_address[1]
    threading.Thread(target=msrv.serve_forever, daemon=True).start()
    try:
        muri = f"mysql://benchuser:secretpass@127.0.0.1:{mport}/benchdb"
        run_cmd([PGBENCH_BIN, "-i", "-s", "1", muri])
        assert msrv.mysql_state.caching_sha2_authed >= 1
        assert any("INSERT INTO pgbench_branches" in q for q in msrv.mysql_state.queries)
        assert any(q.upper() == "COMMIT" for q in msrv.mysql_state.queries)

        for mode in ("simple", "extended", "prepared"):
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "3", "-c", "2", "-j", "2", "-M", mode, muri])
            assert "number of transactions actually processed: 6/6" in r.stdout
        assert msrv.mysql_state.stmt_executions > 0
        assert any("?" in s[0] for s in msrv.mysql_state.prepared_sqls.values())

        # Test pipeline mode + multi-statement TRUNCATE rewrite + ER_LOCK_DEADLOCK (1213 -> SQLSTATE 40001) retry on MySQL
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
            f.write(
                "\\set aid 1\n"
                "TRUNCATE TABLE pgbench_history, pgbench_tellers;\n"
                "BEGIN;\n"
                "\\startpipeline\n"
                "UPDATE pgbench_branches SET bbalance = bbalance + 1 WHERE bid = :aid;\n"
                "UPDATE pgbench_tellers SET tbalance = tbalance + 1 WHERE tid = :aid;\n"
                "\\endpipeline\n"
                "UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid /* deadlock_test */;\n"
                "SELECT :aid AS val_out, 'mysql_ok' AS tag_out \\gset\n"
                "COMMIT;\n"
            )
            mpath = f.name
        try:
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "2", "-c", "1", "--max-tries", "3", "-M", "extended", "-f", mpath, muri])
            assert "number of transactions actually processed: 2/2" in r.stdout
            assert "number of transactions retried: 1" in r.stdout
            assert msrv.mysql_state.multi_stmt_queries >= 1
        finally:
            os.unlink(mpath)

        # Test canonical dataset loading (--load-dataset) with DATE, JSON, and BYTES columns on MySQL
        with tempfile.TemporaryDirectory() as ds_dir:
            create_canonical_test_dataset(ds_dir)
            run_cmd([PGBENCH_BIN, f"--load-dataset={ds_dir}", muri])
            assert any("INSERT INTO custom_items" in q and "X'deadbeef'" in q for q in msrv.mysql_state.queries)
    finally:
        msrv.shutdown()
        msrv.server_close()

class Neo4jState:
    def __init__(self):
        self.lock = threading.Lock()
        self.auth_headers = []
        self.tx_seq = 0
        self.open_txs = set()
        self.committed_txs = 0
        self.rolled_back_txs = 0
        self.statements = []
        self.batch_sizes = []
        self.unwind_rows_loaded = 0
        self.deadlock_once = True

class Neo4jHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):
        pass
    def _send_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "keep-alive")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def do_DELETE(self):
        st = self.server.neo4j_state
        with st.lock:
            st.rolled_back_txs += 1
        self._send_json(200, {"results": [], "errors": []})

    def do_POST(self):
        clen = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(clen).decode("utf-8")) if clen > 0 else {}
        st = self.server.neo4j_state
        auth = self.headers.get("Authorization", "")
        stmts = req.get("statements", [])
        host_hdr = self.headers.get("Host", "127.0.0.1:7474")

        with st.lock:
            if auth:
                st.auth_headers.append(auth)
            if len(stmts) > 0:
                st.batch_sizes.append(len(stmts))

        # Open new transaction: POST /db/<db>/tx
        if self.path.rstrip("/").endswith("/tx"):
            with st.lock:
                st.tx_seq += 1
                tx_id = st.tx_seq
                st.open_txs.add(tx_id)
            commit_url = f"http://{host_hdr}{self.path.rstrip('/')}/{tx_id}/commit"
            self._send_json(201, {"commit": commit_url, "results": [], "errors": []})
            return

        is_commit = self.path.endswith("/commit")
        if is_commit and "/tx/" in self.path and not self.path.endswith("/tx/commit"):
            with st.lock:
                st.committed_txs += 1

        results = []
        for item in stmts:
            cypher = item.get("statement", "").strip()
            params = item.get("parameters", {})
            with st.lock:
                st.statements.append((cypher, params))
                if "rows" in params and isinstance(params["rows"], list):
                    st.unwind_rows_loaded += len(params["rows"])

            upper = cypher.upper()
            if "DEADLOCK_CYPHER_TEST" in upper:
                with st.lock:
                    fail_now = st.deadlock_once
                    st.deadlock_once = False
                if fail_now:
                    self._send_json(200, {
                        "results": results,
                        "errors": [{
                            "code": "Neo.TransientError.Transaction.DeadlockDetected",
                            "message": "LockClient[1] can't wait on resource"
                        }]
                    })
                    return

            if "RETURN COUNT(" in upper:
                results.append({"columns": ["count"], "data": [{"row": [1], "meta": [None]}]})
            elif "RETURN A.ABALANCE" in upper:
                results.append({"columns": ["abalance"], "data": [{"row": [750], "meta": [None]}]})
            elif "RETURN " in upper:
                p1 = params.get("p1", 10)
                results.append({
                    "columns": ["node_id", "driver_tag"],
                    "data": [{"row": [p1, "neo4j_ok"], "meta": [None, None]}]
                })
            else:
                results.append({"columns": [], "data": []})

        resp = {"results": results, "errors": []}
        if not is_commit:
            resp["commit"] = f"http://{host_hdr}{self.path.rstrip('/')}/commit"
        self._send_json(200, resp)

def run_neo4j_tests():
    nsrv = ThreadedHTTPServer(("127.0.0.1", 0), Neo4jHandler)
    nsrv.neo4j_state = Neo4jState()
    nport = nsrv.server_address[1]
    threading.Thread(target=nsrv.serve_forever, daemon=True).start()
    try:
        nuri = f"neo4j://neo4j:secret@127.0.0.1:{nport}/neo4j"
        run_cmd([PGBENCH_BIN, "-i", "-s", "1", nuri])
        assert nsrv.neo4j_state.unwind_rows_loaded > 0
        assert any(a.startswith("Basic ") for a in nsrv.neo4j_state.auth_headers)

        for mode in ("simple", "extended", "prepared"):
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "3", "-c", "2", "-j", "2", "-M", mode, nuri])
            assert "number of transactions actually processed: 6/6" in r.stdout
        assert any("$p1" in s[0] for s in nsrv.neo4j_state.statements)

        # Test explicit transaction lifecycle (BEGIN / COMMIT / ROLLBACK),
        # pipeline multi-statement batching, and Neo.TransientError retry
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
            f.write(
                "\\set pid 42\n"
                "BEGIN;\n"
                "\\startpipeline\n"
                "MATCH (p:Person {id: :pid}) SET p.views = coalesce(p.views, 0) + 1;\n"
                "MATCH (p:Person {id: :pid}) SET p.score = coalesce(p.score, 0) + 5;\n"
                "\\endpipeline\n"
                "MATCH (p:Person {id: :pid}) /* deadlock_cypher_test */ RETURN :pid AS node_id, 'neo4j_ok' AS driver_tag \\gset\n"
                "COMMIT;\n"
            )
            npath = f.name
        try:
            r = run_cmd([PGBENCH_BIN, "-n", "-t", "2", "-c", "1", "--max-tries", "3", "-M", "extended", "-f", npath, nuri])
            assert "number of transactions actually processed: 2/2" in r.stdout
            assert "number of transactions retried: 1" in r.stdout
            assert any(b == 2 for b in nsrv.neo4j_state.batch_sizes)
            assert nsrv.neo4j_state.committed_txs >= 2
            assert nsrv.neo4j_state.rolled_back_txs >= 1
        finally:
            os.unlink(npath)

        # Test canonical dataset loading (--load-dataset) with DATE, JSON, and BYTES columns on Neo4j
        with tempfile.TemporaryDirectory() as ds_dir:
            create_canonical_test_dataset(ds_dir)
            run_cmd([PGBENCH_BIN, f"--load-dataset={ds_dir}", nuri])
            assert any("CREATE (n:`custom_items`)" in s[0] for s in nsrv.neo4j_state.statements)
            assert any(
                "CREATE (n:`custom_items`)" in s[0] and
                s[1].get("rows", [{}])[0].get("raw_bytes") == "\\xdeadbeef"
                for s in nsrv.neo4j_state.statements
            )
            assert any("DETACH DELETE n" in s[0] for s in nsrv.neo4j_state.statements)
    finally:
        nsrv.shutdown()
        nsrv.server_close()

if __name__ == "__main__":
    main()
PYEOF
close $fh;

command_ok([ $python, $mock_script ], 'quack, spanner, mysql, and neo4j drivers end-to-end protocol tests');

# Verify canonical dataset export/load with non-pgbench_% user tables, sequences,
# and DATE / JSON / BYTES column logical types against a real PostgreSQL cluster
my $node = PostgreSQL::Test::Cluster->new('canon_ds');
$node->init(extra => [ '--locale', 'C' ]);
$node->start;

$node->safe_psql('postgres', q{
	CREATE SEQUENCE custom_orders_seq START WITH 5000000;
	SELECT nextval('custom_orders_seq');
	CREATE TABLE custom_orders (
		order_id bigint PRIMARY KEY,
		order_date date NOT NULL,
		metadata jsonb NOT NULL,
		payload bytea NOT NULL
	);
	INSERT INTO custom_orders VALUES
		(1, '2026-03-15', '{"tier":"gold"}', '\xdeadbeef'),
		(2, '2026-04-01', '{"tier":"silver"}', '\x01020304');
	CREATE TABLE perffarm_runs (id int);
	CREATE TABLE pgbench_run_metrics (id int);
});

my $export_dir = "$testdir/exported_canon";
$node->command_ok(
	[ 'pgbench', "--export-dataset=$export_dir", '-p', $node->port, '-h', $node->host, 'postgres' ],
	'pgbench --export-dataset exports non-pgbench_% user tables and sequences');

my $manifest = slurp_file("$export_dir/manifest.json");
like($manifest, qr/"name":\s*"custom_orders"/, 'manifest includes custom_orders user table');
unlike($manifest, qr/perffarm_runs|pgbench_run_metrics/, 'manifest excludes telemetry tables');
like($manifest, qr/"DATE"/, 'manifest maps date to DATE logical type');
like($manifest, qr/"JSON"/, 'manifest maps jsonb to JSON logical type');
like($manifest, qr/"BYTES"/, 'manifest maps bytea to BYTES logical type');
like($manifest, qr/"name":\s*"custom_orders_seq"/, 'manifest includes custom_orders_seq sequence');

$node->safe_psql('postgres', q{
	TRUNCATE custom_orders;
	SELECT setval('custom_orders_seq', 1, false);
});

$node->command_ok(
	[ 'pgbench', "--load-dataset=$export_dir", '-p', $node->port, '-h', $node->host, 'postgres' ],
	'pgbench --load-dataset reloads custom_orders and sequence watermarks');

my $reloaded = $node->safe_psql('postgres',
	q{SELECT order_id, order_date, metadata->>'tier', encode(payload, 'hex') FROM custom_orders ORDER BY order_id});
is($reloaded, "1|2026-03-15|gold|deadbeef\n2|2026-04-01|silver|01020304",
	'reloaded DATE, JSON, and BYTES column values match exported dataset');

my $seq_val = $node->safe_psql('postgres', q{SELECT last_value FROM custom_orders_seq});
is($seq_val, '5000000', 'sequence watermark restored on reload');

$node->stop;

done_testing();
