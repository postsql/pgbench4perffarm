/*-------------------------------------------------------------------------
 *
 * quack_driver.c
 *		Remote DuckDB (Quack HTTP/1.1 + Binary DataChunk / JSON / TSV)
 *		implementation of the PgBenchDbDriver vtable
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/quack_driver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/string.h"
#include "db_driver.h"
#include "engine.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "variable.h"

/*
 * Quack binary DataChunk logical type tags
 */
#define QUACK_TYPE_NULL		0
#define QUACK_TYPE_BOOLEAN	1
#define QUACK_TYPE_INT32	2
#define QUACK_TYPE_INT64	3
#define QUACK_TYPE_DOUBLE	4
#define QUACK_TYPE_VARCHAR	5

typedef struct QuackPreparedStmt
{
	char	   *name;
	char	   *sql;
	int			nparams;
	struct QuackPreparedStmt *next;
} QuackPreparedStmt;

typedef enum QuackPendingKind
{
	QUACK_PENDING_HTTP_QUERY = 0,
	QUACK_PENDING_SYNTHETIC_OK,
	QUACK_PENDING_PIPELINE_SYNC
} QuackPendingKind;

typedef struct QuackPendingOp
{
	QuackPendingKind kind;
	int			tx_effect;		/* 0=none, 1=begin, 2=commit, 3=rollback */
	struct QuackPendingOp *next;
} QuackPendingOp;

struct PgBenchResult
{
	PgBenchExecStatus status;
	char	   *err_msg;
	char		sqlstate[6];
	int			nrows;
	int			ncols;
	char	  **col_names;
	char	 ***cells;			/* cells[row][col], NULL if SQL NULL */
	struct PgBenchResult *next;
};

struct PgBenchConn
{
	char	   *host;
	char	   *port;
	char	   *user;
	char	   *dbname;
	char	   *endpoint_path;
	char	   *auth_token;
	char		session_id[64];

	int			sock;
	PgBenchConnStatus conn_status;
	PgBenchTxStatus tx_status;
	PgBenchPipelineStatus pipe_status;
	bool		pipe_aborted;

	PQExpBufferData err_buf;

	/* Outbound HTTP request buffer */
	char	   *out_buf;
	size_t		out_len;
	size_t		out_sent;
	size_t		out_cap;

	/* Inbound HTTP response buffer */
	char	   *in_buf;
	size_t		in_len;
	size_t		in_cap;

	/* Pending operations in flight */
	QuackPendingOp *op_head;
	QuackPendingOp *op_tail;

	/* Completed results ready for get_result() */
	PgBenchResult *res_head;
	PgBenchResult *res_tail;
	bool		awaiting_result_null;

	/* Prepared statements catalog */
	QuackPreparedStmt *prep_head;
};

struct PgBenchCopyStream
{
	PgBenchConn *conn;
	char	   *table_name;
	int			ncols;
	char	  **col_names;
	PQExpBufferData line_buf;
	PQExpBufferData batch_sql;
	int			batch_rows;
	bool		failed;
};

static uint64 quack_session_seq = 0;

/* -------------------------------------------------------------------------
 * Result construction & destruction helpers
 * -------------------------------------------------------------------------
 */

static PgBenchResult *
quack_make_result(PgBenchExecStatus status, const char *err_msg, const char *sqlstate)
{
	PgBenchResult *res = pg_malloc0_object(PgBenchResult);

	res->status = status;
	if (err_msg != NULL)
		res->err_msg = pg_strdup(err_msg);
	if (sqlstate != NULL && *sqlstate != '\0')
		strlcpy(res->sqlstate, sqlstate, sizeof(res->sqlstate));
	return res;
}

static void
quack_free_result(PgBenchResult *res)
{
	if (res == NULL)
		return;
	pg_free(res->err_msg);
	if (res->col_names != NULL)
	{
		for (int c = 0; c < res->ncols; c++)
			pg_free(res->col_names[c]);
		pg_free(res->col_names);
	}
	if (res->cells != NULL)
	{
		for (int r = 0; r < res->nrows; r++)
		{
			if (res->cells[r] != NULL)
			{
				for (int c = 0; c < res->ncols; c++)
					pg_free(res->cells[r][c]);
				pg_free(res->cells[r]);
			}
		}
		pg_free(res->cells);
	}
	pg_free(res);
}

static void
quack_enqueue_result(PgBenchConn *conn, PgBenchResult *res)
{
	res->next = NULL;
	if (conn->res_tail != NULL)
		conn->res_tail->next = res;
	else
		conn->res_head = res;
	conn->res_tail = res;
}

static void
quack_enqueue_op(PgBenchConn *conn, QuackPendingKind kind, int tx_effect)
{
	QuackPendingOp *op = pg_malloc0_object(QuackPendingOp);

	op->kind = kind;
	op->tx_effect = tx_effect;
	op->next = NULL;
	if (conn->op_tail != NULL)
		conn->op_tail->next = op;
	else
		conn->op_head = op;
	conn->op_tail = op;
}

static QuackPendingOp *
quack_pop_op(PgBenchConn *conn)
{
	QuackPendingOp *op = conn->op_head;

	if (op != NULL)
	{
		conn->op_head = op->next;
		if (conn->op_head == NULL)
			conn->op_tail = NULL;
	}
	return op;
}

/* -------------------------------------------------------------------------
 * URI & Connection Parameter Parsing
 * -------------------------------------------------------------------------
 */

static void
quack_parse_conn_params(PgBenchConn *conn, const char *host, const char *port,
						const char *user, const char *dbname)
{
	const char *eff_host = (host && *host) ? host : getenv("QUACK_HOST");
	const char *eff_port = (port && *port) ? port : getenv("QUACK_PORT");
	const char *eff_user = (user && *user) ? user : "duckdb";
	const char *eff_db = (dbname && *dbname) ? dbname : "memory";
	const char *token_env = getenv("QUACK_API_TOKEN");

	conn->endpoint_path = pg_strdup("/query");
	if (token_env && *token_env)
		conn->auth_token = pg_strdup(token_env);

	if (eff_db != NULL)
	{
		const char *p = NULL;

		if (pg_strncasecmp(eff_db, "quack://", 8) == 0)
			p = eff_db + 8;
		else if (pg_strncasecmp(eff_db, "duckdb://", 9) == 0)
			p = eff_db + 9;
		else if (pg_strncasecmp(eff_db, "http://", 7) == 0)
			p = eff_db + 7;

		if (p != NULL)
		{
			const char *slash = strchr(p, '/');
			const char *qmark = strchr(p, '?');
			const char *host_end = slash ? slash : (qmark ? qmark : p + strlen(p));
			const char *at = memchr(p, '@', host_end - p);
			const char *host_start = at ? (at + 1) : p;
			const char *colon;

			if (at != NULL)
			{
				const char *ucolon = memchr(p, ':', at - p);

				if (ucolon != NULL)
				{
					conn->user = pnstrdup(p, ucolon - p);
					pg_free(conn->auth_token);
					conn->auth_token = pnstrdup(ucolon + 1, at - (ucolon + 1));
				}
				else
					conn->user = pnstrdup(p, at - p);
			}
			else
				conn->user = pg_strdup(eff_user);

			colon = memchr(host_start, ':', host_end - host_start);
			if (colon != NULL)
			{
				conn->host = pnstrdup(host_start, colon - host_start);
				conn->port = pnstrdup(colon + 1, host_end - (colon + 1));
			}
			else
			{
				conn->host = pnstrdup(host_start, host_end - host_start);
				conn->port = pg_strdup(eff_port ? eff_port : "9999");
			}

			if (slash != NULL)
			{
				const char *db_end = qmark ? qmark : (slash + strlen(slash));

				if (db_end > slash + 1)
					conn->dbname = pnstrdup(slash + 1, db_end - (slash + 1));
				else
					conn->dbname = pg_strdup("memory");
			}
			else
				conn->dbname = pg_strdup("memory");

			if (qmark != NULL)
			{
				const char *tok = strstr(qmark + 1, "token=");

				if (tok != NULL)
				{
					const char *tend = strchr(tok + 6, '&');

					pg_free(conn->auth_token);
					conn->auth_token = tend ?
						pnstrdup(tok + 6, tend - (tok + 6)) :
						pg_strdup(tok + 6);
				}
			}

			return;
		}
	}

	conn->host = pg_strdup((eff_host && *eff_host) ? eff_host : "127.0.0.1");
	conn->port = pg_strdup((eff_port && *eff_port) ? eff_port : "9999");
	conn->user = pg_strdup(eff_user);
	conn->dbname = pg_strdup(eff_db);
}

/* -------------------------------------------------------------------------
 * Socket Connection Helpers
 * -------------------------------------------------------------------------
 */

static int
quack_open_socket(PgBenchConn *conn, bool nonblocking, bool *in_progress)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int			sock = -1;
	int			rc;

	if (in_progress)
		*in_progress = false;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rc = getaddrinfo(conn->host, conn->port, &hints, &res);
	if (rc != 0 || res == NULL)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not translate host name \"%s\" port \"%s\": %s\n",
						  conn->host, conn->port, gai_strerror(rc));
		return -1;
	}

	for (rp = res; rp != NULL; rp = rp->ai_next)
	{
		int			one = 1;

		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;

		(void) setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (nonblocking)
		{
			int			flags = fcntl(sock, F_GETFL, 0);

			if (flags >= 0)
				(void) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
		}

		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
		{
			int			flags = fcntl(sock, F_GETFL, 0);

			if (flags >= 0)
				(void) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
			break;
		}

		if (nonblocking && (errno == EINPROGRESS || errno == EWOULDBLOCK))
		{
			if (in_progress)
				*in_progress = true;
			break;
		}

		close(sock);
		sock = -1;
	}

	if (sock < 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "connection to Quack server at \"%s\", port %s failed: %s\n",
						  conn->host, conn->port, strerror(errno));
	}

	freeaddrinfo(res);
	return sock;
}

static PgBenchConn *
quack_alloc_conn(const char *host, const char *port,
				 const char *user, const char *dbname)
{
	PgBenchConn *conn = pg_malloc0_object(PgBenchConn);
	uint64		seq = __atomic_add_fetch(&quack_session_seq, 1, __ATOMIC_RELAXED);

	initPQExpBuffer(&conn->err_buf);
	quack_parse_conn_params(conn, host, port, user, dbname);
	snprintf(conn->session_id, sizeof(conn->session_id),
			 "pgbench-%d-" UINT64_FORMAT, (int) getpid(), seq);

	conn->sock = -1;
	conn->conn_status = PGBENCH_CONN_BAD;
	conn->tx_status = PGBENCH_TX_IDLE;
	conn->pipe_status = PGBENCH_PIPE_OFF;
	conn->in_cap = 16384;
	conn->in_buf = pg_malloc(conn->in_cap);
	conn->out_cap = 8192;
	conn->out_buf = pg_malloc(conn->out_cap);

	return conn;
}

/* -------------------------------------------------------------------------
 * Outbound I/O & Parameter Substitution
 * -------------------------------------------------------------------------
 */

static void quack_drain_non_http_ops(PgBenchConn *conn);

static const char *
quack_skip_sql_ws_and_comments(const char *p)
{
	while (p != NULL && *p != '\0')
	{
		if (isspace((unsigned char) *p))
		{
			p++;
			continue;
		}
		if (p[0] == '-' && p[1] == '-')
		{
			p += 2;
			while (*p != '\0' && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*')
		{
			int			depth = 1;

			p += 2;
			while (*p != '\0' && depth > 0)
			{
				if (p[0] == '/' && p[1] == '*')
				{
					depth++;
					p += 2;
				}
				else if (p[0] == '*' && p[1] == '/')
				{
					depth--;
					p += 2;
				}
				else
					p++;
			}
			continue;
		}
		break;
	}
	return p;
}

static int
quack_classify_tx_sql(const char *sql)
{
	const char *p = quack_skip_sql_ws_and_comments(sql);

	if (p == NULL)
		return 0;

	if (pg_strncasecmp(p, "BEGIN", 5) == 0 &&
		(p[5] == '\0' || p[5] == ';' || isspace((unsigned char) p[5]) ||
		 p[5] == '-' || p[5] == '/'))
		return 1;
	if (pg_strncasecmp(p, "START TRANSACTION", 17) == 0)
		return 1;
	if ((pg_strncasecmp(p, "COMMIT", 6) == 0 &&
		 (p[6] == '\0' || p[6] == ';' || isspace((unsigned char) p[6]) ||
		  p[6] == '-' || p[6] == '/')) ||
		(pg_strncasecmp(p, "END", 3) == 0 &&
		 (p[3] == '\0' || p[3] == ';' || isspace((unsigned char) p[3]) ||
		  p[3] == '-' || p[3] == '/')))
		return 2;
	if (pg_strncasecmp(p, "ROLLBACK", 8) == 0 &&
		(p[8] == '\0' || p[8] == ';' || isspace((unsigned char) p[8]) ||
		 p[8] == '-' || p[8] == '/'))
		return 3;
	return 0;
}

static bool
quack_is_numeric_literal(const char *s)
{
	const char *p = s;
	bool		has_digit = false;
	bool		has_dot = false;

	if (p == NULL || *p == '\0')
		return false;
	if (*p == '-' || *p == '+')
		p++;
	if (*p == '\0')
		return false;
	/* Avoid treating leading-zero strings like "0012" as numeric */
	if (p[0] == '0' && isdigit((unsigned char) p[1]))
		return false;

	while (*p != '\0')
	{
		if (isdigit((unsigned char) *p))
			has_digit = true;
		else if (*p == '.' && !has_dot)
			has_dot = true;
		else
			return false;
		p++;
	}
	return has_digit;
}

static void
quack_append_param_literal(PQExpBuffer buf, const char *text_val,
						   const struct Variable *var)
{
	if (var != NULL)
	{
		switch (var->value.type)
		{
			case PGBT_NULL:
				appendPQExpBufferStr(buf, "NULL");
				return;
			case PGBT_INT:
				appendPQExpBuffer(buf, INT64_FORMAT, var->value.u.ival);
				return;
			case PGBT_DOUBLE:
				appendPQExpBuffer(buf, "%.17g", var->value.u.dval);
				return;
			case PGBT_BOOLEAN:
				appendPQExpBufferStr(buf, var->value.u.bval ? "TRUE" : "FALSE");
				return;
			default:
				break;
		}
	}

	if (text_val == NULL)
	{
		appendPQExpBufferStr(buf, "NULL");
		return;
	}

	if (quack_is_numeric_literal(text_val))
	{
		appendPQExpBufferStr(buf, text_val);
		return;
	}

	appendPQExpBufferChar(buf, '\'');
	for (const char *p = text_val; *p != '\0'; p++)
	{
		if (*p == '\'')
			appendPQExpBufferStr(buf, "''");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');
}

static char *
quack_bind_sql_params(const char *sql, int nparams,
					  const char *const *param_texts,
					  const struct Variable *const *param_vars)
{
	PQExpBufferData buf;
	const char *p = sql;
	bool		in_squote = false;
	bool		in_dquote = false;

	if (nparams <= 0)
		return pg_strdup(sql);

	initPQExpBuffer(&buf);
	while (*p != '\0')
	{
		if (!in_squote && !in_dquote)
		{
			/* Skip SQL line comments: -- ... \n */
			if (p[0] == '-' && p[1] == '-')
			{
				while (*p != '\0' && *p != '\n')
					appendPQExpBufferChar(&buf, *p++);
				continue;
			}
			/* Skip SQL block comments (including nested) */
			if (p[0] == '/' && p[1] == '*')
			{
				int			depth = 1;

				appendPQExpBufferChar(&buf, *p++);
				appendPQExpBufferChar(&buf, *p++);
				while (*p != '\0' && depth > 0)
				{
					if (p[0] == '/' && p[1] == '*')
					{
						depth++;
						appendPQExpBufferChar(&buf, *p++);
						appendPQExpBufferChar(&buf, *p++);
					}
					else if (p[0] == '*' && p[1] == '/')
					{
						depth--;
						appendPQExpBufferChar(&buf, *p++);
						appendPQExpBufferChar(&buf, *p++);
					}
					else
						appendPQExpBufferChar(&buf, *p++);
				}
				continue;
			}
			/* Skip dollar-quoted strings: $tag$...$tag$ */
			if (*p == '$' &&
				(p[1] == '$' || isalpha((unsigned char) p[1]) || p[1] == '_'))
			{
				const char *tag_end = p + 1;

				while (*tag_end != '\0' &&
					   (isalnum((unsigned char) *tag_end) || *tag_end == '_'))
					tag_end++;
				if (*tag_end == '$')
				{
					size_t		tag_len = (size_t) (tag_end - p + 1);
					const char *close_tag = strstr(tag_end + 1, p);

					if (close_tag != NULL)
					{
						/* find exact tag_len match */
						const char *scan = tag_end + 1;

						while (*scan != '\0')
						{
							if (*scan == '$' && strncmp(scan, p, tag_len) == 0)
							{
								appendBinaryPQExpBuffer(&buf, p, (scan - p) + tag_len);
								p = scan + tag_len;
								break;
							}
							scan++;
						}
						if (*scan != '\0')
							continue;
					}
				}
			}
		}

		if (*p == '\'' && !in_dquote)
		{
			in_squote = !in_squote;
			appendPQExpBufferChar(&buf, *p++);
			continue;
		}
		if (*p == '"' && !in_squote)
		{
			in_dquote = !in_dquote;
			appendPQExpBufferChar(&buf, *p++);
			continue;
		}
		if (!in_squote && !in_dquote && *p == '$' &&
			isdigit((unsigned char) p[1]))
		{
			char	   *endp = NULL;
			long		idx = strtol(p + 1, &endp, 10);

			if (endp > p + 1 && idx >= 1 && idx <= nparams)
			{
				const char *tval = param_texts ? param_texts[idx - 1] : NULL;
				const struct Variable *v = param_vars ? param_vars[idx - 1] : NULL;

				quack_append_param_literal(&buf, tval, v);
				p = endp;
				continue;
			}
		}
		appendPQExpBufferChar(&buf, *p++);
	}
	return buf.data;
}

static bool
quack_flush_out(PgBenchConn *conn)
{
	while (conn->out_sent < conn->out_len)
	{
		ssize_t		n;

#ifdef MSG_NOSIGNAL
		n = send(conn->sock, conn->out_buf + conn->out_sent,
				 conn->out_len - conn->out_sent, MSG_NOSIGNAL);
#else
		n = send(conn->sock, conn->out_buf + conn->out_sent,
				 conn->out_len - conn->out_sent, 0);
#endif
		if (n > 0)
		{
			conn->out_sent += (size_t) n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			/*
			 * Because pgbench's event loop polls only POLLIN while waiting
			 * for query results, we must finish flushing outbound bytes (while
			 * concurrently draining any readable responses into in_buf to
			 * avoid TCP full-duplex deadlock during HTTP pipelining).
			 */
			fd_set		rfds;
			fd_set		wfds;

			FD_ZERO(&rfds);
			FD_ZERO(&wfds);
			FD_SET(conn->sock, &rfds);
			FD_SET(conn->sock, &wfds);

			if (select(conn->sock + 1, &rfds, &wfds, NULL, NULL) < 0)
			{
				if (errno == EINTR)
					continue;
				printfPQExpBuffer(&conn->err_buf,
								  "Quack socket select failed: %s\n", strerror(errno));
				return false;
			}
			if (FD_ISSET(conn->sock, &rfds))
			{
				ssize_t		rn;

				if (conn->in_len + 4096 > conn->in_cap)
				{
					conn->in_cap *= 2;
					conn->in_buf = pg_realloc(conn->in_buf, conn->in_cap);
				}
				rn = recv(conn->sock, conn->in_buf + conn->in_len,
						  conn->in_cap - conn->in_len, 0);
				if (rn > 0)
					conn->in_len += (size_t) rn;
			}
			continue;
		}

		printfPQExpBuffer(&conn->err_buf,
						  "Quack socket send failed: %s\n", strerror(errno));
		return false;
	}

	conn->out_len = 0;
	conn->out_sent = 0;
	return true;
}

static bool
quack_queue_http_query(PgBenchConn *conn, const char *sql)
{
	PQExpBufferData req;
	size_t		body_len = strlen(sql);
	int			tx_effect;

	if (conn->sock < 0)
	{
		conn->sock = quack_open_socket(conn, false, NULL);
		if (conn->sock < 0)
			return false;
		conn->conn_status = PGBENCH_CONN_OK;
	}

	/* Reset result-cycle boundary if starting a new query outside pipeline */
	if (conn->pipe_status == PGBENCH_PIPE_OFF)
		conn->awaiting_result_null = false;
	else if (conn->pipe_aborted)
	{
		/* Pipeline already aborted; synthesize PGBENCH_RES_PIPELINE_ABORTED */
		quack_enqueue_op(conn, QUACK_PENDING_SYNTHETIC_OK, 0);
		quack_drain_non_http_ops(conn);
		return true;
	}

	tx_effect = quack_classify_tx_sql(sql);

	initPQExpBuffer(&req);
	printfPQExpBuffer(&req,
					  "POST %s HTTP/1.1\r\n"
					  "Host: %s:%s\r\n"
					  "Connection: keep-alive\r\n"
					  "Content-Type: application/sql\r\n"
					  "Accept: application/duckdb, application/json, text/tab-separated-values\r\n"
					  "X-DuckDB-Database: %s\r\n"
					  "X-DuckDB-User: %s\r\n"
					  "X-DuckDB-Connection-Id: %s\r\n"
					  "Content-Length: %zu\r\n",
					  conn->endpoint_path,
					  conn->host, conn->port,
					  conn->dbname,
					  conn->user,
					  conn->session_id,
					  body_len);
	if (conn->auth_token != NULL)
		appendPQExpBuffer(&req, "Authorization: Bearer %s\r\n", conn->auth_token);
	appendPQExpBufferStr(&req, "\r\n");
	appendBinaryPQExpBuffer(&req, sql, body_len);

	if (conn->out_sent > 0 && conn->out_sent < conn->out_len)
	{
		memmove(conn->out_buf, conn->out_buf + conn->out_sent,
				conn->out_len - conn->out_sent);
		conn->out_len -= conn->out_sent;
		conn->out_sent = 0;
	}
	else if (conn->out_sent >= conn->out_len)
	{
		conn->out_len = 0;
		conn->out_sent = 0;
	}

	if (conn->out_len + req.len > conn->out_cap)
	{
		while (conn->out_len + req.len > conn->out_cap)
			conn->out_cap *= 2;
		conn->out_buf = pg_realloc(conn->out_buf, conn->out_cap);
	}
	memcpy(conn->out_buf + conn->out_len, req.data, req.len);
	conn->out_len += req.len;
	termPQExpBuffer(&req);

	quack_enqueue_op(conn, QUACK_PENDING_HTTP_QUERY, tx_effect);
	return quack_flush_out(conn);
}

/* -------------------------------------------------------------------------
 * Response Body Decoders (Binary Quack DataChunk, JSON, and TSV)
 * -------------------------------------------------------------------------
 */

static uint32
quack_read_u32_le(const unsigned char *p)
{
	return ((uint32) p[0]) |
		(((uint32) p[1]) << 8) |
		(((uint32) p[2]) << 16) |
		(((uint32) p[3]) << 24);
}

static uint16
quack_read_u16_le(const unsigned char *p)
{
	return ((uint16) p[0]) | (((uint16) p[1]) << 8);
}

static int64
quack_read_i64_le(const unsigned char *p)
{
	uint64		v = ((uint64) quack_read_u32_le(p)) |
		(((uint64) quack_read_u32_le(p + 4)) << 32);

	return (int64) v;
}

/*
 * Parse binary Quack DataChunk frame:
 *   Magic: "QUAK" (4 bytes)
 *   Status: 0 = COMMAND_OK, 1 = TUPLES_OK, 2 = ERROR (1 byte)
 *   If ERROR:
 *     sqlstate: 5 bytes
 *     msg_len: uint32_le
 *     msg: msg_len bytes
 *   If COMMAND_OK / TUPLES_OK:
 *     ncols: uint32_le
 *     nrows: uint32_le
 *     For each col in 0..ncols-1:
 *       name_len: uint16_le, name: bytes, type_id: uint8
 *     For each col in 0..ncols-1:
 *       validity_bitmap: ceil(nrows / 8) bytes (bit=1 valid, 0 null)
 *       values:
 *         BOOLEAN: nrows bytes (0/1)
 *         INT32: nrows * 4 bytes
 *         INT64: nrows * 8 bytes
 *         DOUBLE: nrows * 8 bytes (IEEE-754)
 *         VARCHAR: for each row: uint32_le len + len bytes
 */
static PgBenchResult *
quack_parse_binary_datachunk(const unsigned char *buf, size_t len,
							 const char *hdr_sqlstate)
{
	uint8		status_tag;
	uint32		ncols;
	uint32		nrows;
	size_t		off = 4;
	uint8	   *col_types = NULL;
	PgBenchResult *res;

	if (len < 5)
		return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
								 "truncated Quack binary frame", "08P01");

	status_tag = buf[off++];
	if (status_tag == 2)
	{
		char		st[6] = "XX000";
		uint32		mlen = 0;
		char	   *msg;

		if (off + 9 <= len)
		{
			memcpy(st, buf + off, 5);
			st[5] = '\0';
			off += 5;
			mlen = quack_read_u32_le(buf + off);
			off += 4;
		}
		if (off + mlen > len)
			mlen = (uint32) (len - off);
		msg = pnstrdup((const char *) (buf + off), mlen);
		res = quack_make_result(PGBENCH_RES_FATAL_ERROR, msg,
								(hdr_sqlstate && *hdr_sqlstate) ? hdr_sqlstate : st);
		pg_free(msg);
		return res;
	}

	if (off + 8 > len)
		return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
								 "truncated Quack DataChunk header", "08P01");

	ncols = quack_read_u32_le(buf + off);
	off += 4;
	nrows = quack_read_u32_le(buf + off);
	off += 4;

	if (status_tag == 0 && ncols == 0)
	{
		res = quack_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
		res->nrows = (int) nrows;
		return res;
	}

	res = quack_make_result(PGBENCH_RES_TUPLES_OK, NULL, NULL);
	res->ncols = (int) ncols;
	res->nrows = (int) nrows;
	res->col_names = pg_malloc0_array(char *, ncols);
	col_types = pg_malloc0_array(uint8, ncols);

	for (uint32 c = 0; c < ncols; c++)
	{
		uint16		nlen;

		if (off + 2 > len)
		{
			pg_free(col_types);
			quack_free_result(res);
			return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
									 "truncated Quack column header", "08P01");
		}
		nlen = quack_read_u16_le(buf + off);
		off += 2;
		if (off + nlen + 1 > len)
		{
			pg_free(col_types);
			quack_free_result(res);
			return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
									 "truncated Quack column name", "08P01");
		}
		res->col_names[c] = pnstrdup((const char *) (buf + off), nlen);
		off += nlen;
		col_types[c] = buf[off++];
	}

	if (nrows > 0)
	{
		res->cells = pg_malloc0_array(char **, nrows);
		for (uint32 r = 0; r < nrows; r++)
			res->cells[r] = pg_malloc0_array(char *, ncols);
	}

	for (uint32 c = 0; c < ncols; c++)
	{
		size_t		bmap_bytes = (nrows + 7) / 8;
		const unsigned char *bmap;

		if (off + bmap_bytes > len)
		{
			pg_free(col_types);
			quack_free_result(res);
			return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
									 "truncated Quack validity bitmap", "08P01");
		}
		bmap = buf + off;
		off += bmap_bytes;

		for (uint32 r = 0; r < nrows; r++)
		{
			bool		is_valid = (bmap[r / 8] & (1u << (r % 8))) != 0;
			char		numbuf[64];

			switch (col_types[c])
			{
				case QUACK_TYPE_NULL:
					break;
				case QUACK_TYPE_BOOLEAN:
					if (off + 1 > len)
						goto truncated;
					if (is_valid)
						res->cells[r][c] = pg_strdup(buf[off] ? "t" : "f");
					off += 1;
					break;
				case QUACK_TYPE_INT32:
					if (off + 4 > len)
						goto truncated;
					if (is_valid)
					{
						int32		v = (int32) quack_read_u32_le(buf + off);

						snprintf(numbuf, sizeof(numbuf), "%d", v);
						res->cells[r][c] = pg_strdup(numbuf);
					}
					off += 4;
					break;
				case QUACK_TYPE_INT64:
					if (off + 8 > len)
						goto truncated;
					if (is_valid)
					{
						int64		v = quack_read_i64_le(buf + off);

						snprintf(numbuf, sizeof(numbuf), INT64_FORMAT, v);
						res->cells[r][c] = pg_strdup(numbuf);
					}
					off += 8;
					break;
				case QUACK_TYPE_DOUBLE:
					if (off + 8 > len)
						goto truncated;
					if (is_valid)
					{
						double		d;

						memcpy(&d, buf + off, 8);
						snprintf(numbuf, sizeof(numbuf), "%.15g", d);
						res->cells[r][c] = pg_strdup(numbuf);
					}
					off += 8;
					break;
				case QUACK_TYPE_VARCHAR:
				default:
					{
						uint32		slen;

						if (off + 4 > len)
							goto truncated;
						slen = quack_read_u32_le(buf + off);
						off += 4;
						if (off + slen > len)
							goto truncated;
						if (is_valid)
							res->cells[r][c] = pnstrdup((const char *) (buf + off), slen);
						off += slen;
					}
					break;
			}
		}
	}

	pg_free(col_types);
	return res;

truncated:
	pg_free(col_types);
	quack_free_result(res);
	return quack_make_result(PGBENCH_RES_BAD_RESPONSE,
							 "truncated Quack vector payload", "08P01");
}

/*
 * Parse TSV tabular body:
 *   Line 1: col1\tcol2\t...
 *   Line 2..N: val1\tval2\t... (\N represents SQL NULL)
 *   If body starts with "COMMAND_OK", returns PGBENCH_RES_COMMAND_OK.
 */
static PgBenchResult *
quack_parse_tsv_body(const char *body, size_t len, int http_status,
					 const char *hdr_sqlstate)
{
	PgBenchResult *res;
	const char *p = body;
	const char *end = body + len;
	const char *nl;
	int			ncols = 0;
	int			row_cap = 0;

	if (http_status >= 400)
	{
		char	   *emsg = pnstrdup(body, len);
		const char *st = (hdr_sqlstate && *hdr_sqlstate) ? hdr_sqlstate :
			(http_status == 409 ? "40001" : "XX000");

		res = quack_make_result(PGBENCH_RES_FATAL_ERROR, emsg, st);
		pg_free(emsg);
		return res;
	}

	if (len == 0 || (len >= 10 && strncmp(body, "COMMAND_OK", 10) == 0))
		return quack_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);

	nl = memchr(p, '\n', end - p);
	if (nl == NULL)
		nl = end;

	/* Count columns in header line */
	ncols = 1;
	for (const char *c = p; c < nl; c++)
	{
		if (*c == '\t')
			ncols++;
	}

	res = quack_make_result(PGBENCH_RES_TUPLES_OK, NULL, NULL);
	res->ncols = ncols;
	res->col_names = pg_malloc0_array(char *, ncols);

	{
		const char *col_start = p;
		int			col_idx = 0;

		for (const char *c = p; c <= nl; c++)
		{
			if (c == nl || *c == '\t')
			{
				size_t		clen = c - col_start;

				if (clen > 0 && col_start[clen - 1] == '\r')
					clen--;
				res->col_names[col_idx++] = pnstrdup(col_start, clen);
				col_start = c + 1;
			}
		}
	}

	p = (nl < end) ? (nl + 1) : end;
	while (p < end)
	{
		char	  **row_cells;
		const char *col_start;
		int			col_idx = 0;

		nl = memchr(p, '\n', end - p);
		if (nl == NULL)
			nl = end;

		/* Skip trailing empty line */
		if (nl == p || (nl == p + 1 && *p == '\r'))
		{
			p = (nl < end) ? (nl + 1) : end;
			continue;
		}

		if (res->nrows >= row_cap)
		{
			row_cap = (row_cap == 0) ? 16 : (row_cap * 2);
			res->cells = pg_realloc_array(res->cells, char **, row_cap);
		}
		row_cells = pg_malloc0_array(char *, ncols);
		col_start = p;

		for (const char *c = p; c <= nl && col_idx < ncols; c++)
		{
			if (c == nl || *c == '\t')
			{
				size_t		clen = c - col_start;

				if (clen > 0 && col_start[clen - 1] == '\r')
					clen--;
				if (!(clen == 2 && col_start[0] == '\\' && col_start[1] == 'N'))
					row_cells[col_idx] = pnstrdup(col_start, clen);
				col_idx++;
				col_start = c + 1;
			}
		}
		res->cells[res->nrows++] = row_cells;
		p = (nl < end) ? (nl + 1) : end;
	}

	return res;
}

/*
 * Lightweight JSON helpers for Quack JSON responses:
 *   {"status": "ok"}
 *   {"columns": ["c1", "c2"], "rows": [[1, "a"], [2, null]]}
 *   {"error": "message", "sqlstate": "40001"}
 */
static const char *
quack_json_skip_ws(const char *p, const char *end)
{
	while (p < end && isspace((unsigned char) *p))
		p++;
	return p;
}

static char *
quack_json_parse_string(const char **pp, const char *end)
{
	const char *p = *pp;
	PQExpBufferData buf;

	if (p >= end || *p != '"')
		return NULL;
	p++;
	initPQExpBuffer(&buf);
	while (p < end && *p != '"')
	{
		if (*p == '\\' && p + 1 < end)
		{
			p++;
			switch (*p)
			{
				case 'n':
					appendPQExpBufferChar(&buf, '\n');
					break;
				case 'r':
					appendPQExpBufferChar(&buf, '\r');
					break;
				case 't':
					appendPQExpBufferChar(&buf, '\t');
					break;
				case '"':
				case '\\':
				case '/':
					appendPQExpBufferChar(&buf, *p);
					break;
				default:
					appendPQExpBufferChar(&buf, *p);
					break;
			}
			p++;
		}
		else
			appendPQExpBufferChar(&buf, *p++);
	}
	if (p < end && *p == '"')
		p++;
	*pp = p;
	return buf.data;
}

static const char *
quack_json_find_key(const char *json, const char *end, const char *key)
{
	size_t		klen = strlen(key);
	const char *p = json;

	while (p < end)
	{
		if (*p == '"')
		{
			if ((size_t) (end - (p + 1)) > klen &&
				memcmp(p + 1, key, klen) == 0 &&
				p[1 + klen] == '"')
			{
				const char *after = quack_json_skip_ws(p + 2 + klen, end);

				if (after < end && *after == ':')
					return quack_json_skip_ws(after + 1, end);
			}
			/* skip string */
			p++;
			while (p < end && *p != '"')
			{
				if (*p == '\\' && p + 1 < end)
					p += 2;
				else
					p++;
			}
		}
		p++;
	}
	return NULL;
}

static PgBenchResult *
quack_parse_json_body(const char *body, size_t len, int http_status,
					  const char *hdr_sqlstate)
{
	const char *end = body + len;
	const char *err_val = quack_json_find_key(body, end, "error");
	const char *cols_val;
	const char *rows_val;
	PgBenchResult *res;

	if (http_status >= 400 || (err_val != NULL && *err_val == '"'))
	{
		char	   *emsg = NULL;
		char	   *jstate = NULL;
		const char *st_val = quack_json_find_key(body, end, "sqlstate");

		if (err_val != NULL && *err_val == '"')
			emsg = quack_json_parse_string(&err_val, end);
		else
			emsg = pnstrdup(body, len);

		if (st_val != NULL && *st_val == '"')
			jstate = quack_json_parse_string(&st_val, end);

		res = quack_make_result(PGBENCH_RES_FATAL_ERROR, emsg,
								jstate ? jstate :
								((hdr_sqlstate && *hdr_sqlstate) ? hdr_sqlstate :
								 (http_status == 409 ? "40001" : "XX000")));
		pg_free(emsg);
		pg_free(jstate);
		return res;
	}

	cols_val = quack_json_find_key(body, end, "columns");
	rows_val = quack_json_find_key(body, end, "rows");
	if (cols_val == NULL || *cols_val != '[')
	{
		const char *ra_val = quack_json_find_key(body, end, "rows_affected");

		res = quack_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
		if (ra_val != NULL && (isdigit((unsigned char) *ra_val) || *ra_val == '-'))
			res->nrows = atoi(ra_val);
		return res;
	}

	res = quack_make_result(PGBENCH_RES_TUPLES_OK, NULL, NULL);
	{
		const char *p = quack_json_skip_ws(cols_val + 1, end);
		int			col_cap = 8;

		res->col_names = pg_malloc0_array(char *, col_cap);
		while (p < end && *p != ']')
		{
			if (*p == '"')
			{
				if (res->ncols >= col_cap)
				{
					col_cap *= 2;
					res->col_names = pg_realloc_array(res->col_names, char *, col_cap);
				}
				res->col_names[res->ncols++] = quack_json_parse_string(&p, end);
			}
			else
				p++;
			p = quack_json_skip_ws(p, end);
			if (p < end && *p == ',')
				p = quack_json_skip_ws(p + 1, end);
		}
	}

	if (rows_val != NULL && *rows_val == '[' && res->ncols > 0)
	{
		const char *p = quack_json_skip_ws(rows_val + 1, end);
		int			row_cap = 16;

		res->cells = pg_malloc0_array(char **, row_cap);
		while (p < end && *p != ']')
		{
			if (*p == '[')
			{
				char	  **row_cells = pg_malloc0_array(char *, res->ncols);
				int			cidx = 0;

				p = quack_json_skip_ws(p + 1, end);
				while (p < end && *p != ']')
				{
					if (*p == '"')
					{
						char	   *s = quack_json_parse_string(&p, end);

						if (cidx < res->ncols)
							row_cells[cidx++] = s;
						else
							pg_free(s);
					}
					else if (end - p >= 4 && strncmp(p, "null", 4) == 0)
					{
						if (cidx < res->ncols)
							row_cells[cidx++] = NULL;
						p += 4;
					}
					else if (end - p >= 4 && strncmp(p, "true", 4) == 0)
					{
						if (cidx < res->ncols)
							row_cells[cidx++] = pg_strdup("t");
						p += 4;
					}
					else if (end - p >= 5 && strncmp(p, "false", 5) == 0)
					{
						if (cidx < res->ncols)
							row_cells[cidx++] = pg_strdup("f");
						p += 5;
					}
					else
					{
						const char *vstart = p;

						while (p < end && *p != ',' && *p != ']' &&
							   !isspace((unsigned char) *p))
							p++;
						if (cidx < res->ncols)
							row_cells[cidx++] = pnstrdup(vstart, p - vstart);
					}
					p = quack_json_skip_ws(p, end);
					if (p < end && *p == ',')
						p = quack_json_skip_ws(p + 1, end);
				}
				if (p < end && *p == ']')
					p++;
				if (res->nrows >= row_cap)
				{
					row_cap *= 2;
					res->cells = pg_realloc_array(res->cells, char **, row_cap);
				}
				res->cells[res->nrows++] = row_cells;
			}
			else
				p++;
			p = quack_json_skip_ws(p, end);
			if (p < end && *p == ',')
				p = quack_json_skip_ws(p + 1, end);
		}
	}

	return res;
}

/* -------------------------------------------------------------------------
 * HTTP/1.1 Response Framing Parser
 * -------------------------------------------------------------------------
 */

static void
quack_drain_non_http_ops(PgBenchConn *conn)
{
	while (conn->op_head != NULL &&
		   conn->op_head->kind != QUACK_PENDING_HTTP_QUERY)
	{
		QuackPendingOp *op = quack_pop_op(conn);

		if (op->kind == QUACK_PENDING_SYNTHETIC_OK)
		{
			if (conn->pipe_aborted)
				quack_enqueue_result(conn,
									 quack_make_result(PGBENCH_RES_PIPELINE_ABORTED,
													   NULL, NULL));
			else
				quack_enqueue_result(conn,
									 quack_make_result(PGBENCH_RES_COMMAND_OK,
													   NULL, NULL));
		}
		else if (op->kind == QUACK_PENDING_PIPELINE_SYNC)
		{
			conn->pipe_aborted = false;
			if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
				conn->pipe_status = PGBENCH_PIPE_ON;
			quack_enqueue_result(conn,
								 quack_make_result(PGBENCH_RES_PIPELINE_SYNC,
												   NULL, NULL));
		}
		pg_free(op);
	}
}

static bool
quack_try_parse_http_responses(PgBenchConn *conn)
{
	quack_drain_non_http_ops(conn);

	while (conn->in_len > 0)
	{
		const char *hdr_end = NULL;
		size_t		hdr_len;
		int			http_status = 200;
		size_t		content_len = 0;
		bool		has_content_len = false;
		bool		is_chunked = false;
		char		ctype[64] = "";
		char		sqlstate[6] = "";
		char		tx_hdr[16] = "";
		const char *body_ptr;
		size_t		total_frame_len = 0;
		char	   *decoded_chunk_body = NULL;
		size_t		decoded_len = 0;
		PgBenchResult *res;
		QuackPendingOp *op;

		for (size_t i = 0; i + 3 < conn->in_len; i++)
		{
			if (conn->in_buf[i] == '\r' && conn->in_buf[i + 1] == '\n' &&
				conn->in_buf[i + 2] == '\r' && conn->in_buf[i + 3] == '\n')
			{
				hdr_end = conn->in_buf + i;
				break;
			}
		}
		if (hdr_end == NULL)
			break;

		hdr_len = (size_t) (hdr_end - conn->in_buf) + 4;
		body_ptr = conn->in_buf + hdr_len;

		/* Parse status line & headers */
		{
			const char *line = conn->in_buf;
			const char *sp = memchr(line, ' ', hdr_end - line);

			if (sp != NULL)
				http_status = atoi(sp + 1);

			while (line < hdr_end)
			{
				const char *eol = memchr(line, '\r', hdr_end - line);

				if (eol == NULL)
					eol = hdr_end;

				if (pg_strncasecmp(line, "Content-Length:", 15) == 0)
				{
					const char *v = line + 15;

					while (v < eol && isspace((unsigned char) *v))
						v++;
					content_len = (size_t) strtoull(v, NULL, 10);
					has_content_len = true;
				}
				else if (pg_strncasecmp(line, "Transfer-Encoding:", 18) == 0)
				{
					const char *v = line + 18;

					while (v < eol && isspace((unsigned char) *v))
						v++;
					if (pg_strncasecmp(v, "chunked", 7) == 0)
						is_chunked = true;
				}
				else if (pg_strncasecmp(line, "Content-Type:", 13) == 0)
				{
					const char *v = line + 13;
					size_t		vlen;

					while (v < eol && isspace((unsigned char) *v))
						v++;
					vlen = (size_t) (eol - v);
					if (vlen >= sizeof(ctype))
						vlen = sizeof(ctype) - 1;
					memcpy(ctype, v, vlen);
					ctype[vlen] = '\0';
				}
				else if (pg_strncasecmp(line, "X-DuckDB-SQLState:", 18) == 0 ||
						 pg_strncasecmp(line, "X-Quack-SQLState:", 17) == 0)
				{
					const char *v = strchr(line, ':') + 1;

					while (v < eol && isspace((unsigned char) *v))
						v++;
					if (eol - v >= 5)
					{
						memcpy(sqlstate, v, 5);
						sqlstate[5] = '\0';
					}
				}
				else if (pg_strncasecmp(line, "X-DuckDB-Tx-Status:", 19) == 0)
				{
					const char *v = line + 19;
					size_t		vlen;

					while (v < eol && isspace((unsigned char) *v))
						v++;
					vlen = (size_t) (eol - v);
					if (vlen >= sizeof(tx_hdr))
						vlen = sizeof(tx_hdr) - 1;
					memcpy(tx_hdr, v, vlen);
					tx_hdr[vlen] = '\0';
				}

				line = (eol + 2 <= hdr_end) ? (eol + 2) : hdr_end;
			}
		}

		if (is_chunked)
		{
			size_t		pos = hdr_len;
			PQExpBufferData cbuf;
			bool		complete = false;

			initPQExpBuffer(&cbuf);
			while (pos < conn->in_len)
			{
				const char *line_start = conn->in_buf + pos;
				const char *crlf = NULL;
				char	   *endp = NULL;
				size_t		chunk_sz;

				for (size_t k = pos; k + 1 < conn->in_len; k++)
				{
					if (conn->in_buf[k] == '\r' && conn->in_buf[k + 1] == '\n')
					{
						crlf = conn->in_buf + k;
						break;
					}
				}
				if (crlf == NULL)
					break;

				chunk_sz = (size_t) strtoull(line_start, &endp, 16);
				pos = (size_t) (crlf - conn->in_buf) + 2;
				if (chunk_sz == 0)
				{
					if (pos + 2 <= conn->in_len)
					{
						pos += 2;
						total_frame_len = pos;
						complete = true;
					}
					break;
				}
				if (pos + chunk_sz + 2 > conn->in_len)
					break;
				appendBinaryPQExpBuffer(&cbuf, conn->in_buf + pos, chunk_sz);
				pos += chunk_sz + 2;
			}

			if (!complete)
			{
				termPQExpBuffer(&cbuf);
				break;
			}
			decoded_chunk_body = cbuf.data;
			decoded_len = cbuf.len;
			body_ptr = decoded_chunk_body;
		}
		else if (has_content_len)
		{
			if (conn->in_len < hdr_len + content_len)
				break;
			total_frame_len = hdr_len + content_len;
			decoded_len = content_len;
		}
		else
		{
			total_frame_len = hdr_len;
			decoded_len = 0;
		}

		/* Decode body into PgBenchResult */
		if (decoded_len >= 4 && memcmp(body_ptr, "QUAK", 4) == 0)
		{
			res = quack_parse_binary_datachunk((const unsigned char *) body_ptr,
											   decoded_len, sqlstate);
		}
		else if (strstr(ctype, "application/json") != NULL ||
				 (decoded_len > 0 && (body_ptr[0] == '{' || body_ptr[0] == '[')))
		{
			res = quack_parse_json_body(body_ptr, decoded_len,
										http_status, sqlstate);
		}
		else
		{
			res = quack_parse_tsv_body(body_ptr, decoded_len,
									   http_status, sqlstate);
		}

		pg_free(decoded_chunk_body);

		/* Consume frame from input buffer */
		if (total_frame_len < conn->in_len)
			memmove(conn->in_buf, conn->in_buf + total_frame_len,
					conn->in_len - total_frame_len);
		conn->in_len -= total_frame_len;

		/* Pop corresponding pending operation and update tx/pipeline state */
		op = quack_pop_op(conn);
		if (conn->pipe_aborted)
		{
			quack_free_result(res);
			res = quack_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
		}
		else if (res->status == PGBENCH_RES_FATAL_ERROR)
		{
			resetPQExpBuffer(&conn->err_buf);
			if (res->err_msg != NULL)
				appendPQExpBuffer(&conn->err_buf, "%s\n", res->err_msg);
			if (conn->tx_status == PGBENCH_TX_INTRANS)
				conn->tx_status = PGBENCH_TX_INERROR;
			if (conn->pipe_status == PGBENCH_PIPE_ON)
			{
				conn->pipe_status = PGBENCH_PIPE_ABORTED;
				conn->pipe_aborted = true;
			}
		}
		else if (op != NULL)
		{
			if (op->tx_effect == 1)
				conn->tx_status = PGBENCH_TX_INTRANS;
			else if (op->tx_effect == 2 || op->tx_effect == 3)
				conn->tx_status = PGBENCH_TX_IDLE;
		}

		if (tx_hdr[0] != '\0')
		{
			if (pg_strcasecmp(tx_hdr, "intrans") == 0)
				conn->tx_status = PGBENCH_TX_INTRANS;
			else if (pg_strcasecmp(tx_hdr, "inerror") == 0)
				conn->tx_status = PGBENCH_TX_INERROR;
			else if (pg_strcasecmp(tx_hdr, "idle") == 0)
				conn->tx_status = PGBENCH_TX_IDLE;
		}

		pg_free(op);
		quack_enqueue_result(conn, res);
		quack_drain_non_http_ops(conn);
	}

	return true;
}

/* -------------------------------------------------------------------------
 * PgBenchDbDriver Vtable Methods for Quack
 * -------------------------------------------------------------------------
 */

static bool
quack_init_global(const char *conninfo, int nthreads_arg, int nclients_arg)
{
	(void) conninfo;
	(void) nthreads_arg;
	(void) nclients_arg;
	return true;
}

static void
quack_shutdown_global(void)
{
}

static PgBenchConn *
quack_connect_sync(const char *host, const char *port,
				   const char *user, const char *dbname)
{
	PgBenchConn *conn = quack_alloc_conn(host, port, user, dbname);

	conn->sock = quack_open_socket(conn, false, NULL);
	if (conn->sock < 0)
	{
		pg_log_error("%s", conn->err_buf.data);
		active_driver->finish(conn);
		return NULL;
	}
	conn->conn_status = PGBENCH_CONN_OK;
	return conn;
}

static PgBenchConn *
quack_connect_start(const char *host, const char *port,
					const char *user, const char *dbname)
{
	PgBenchConn *conn = quack_alloc_conn(host, port, user, dbname);
	bool		in_prog = false;

	conn->sock = quack_open_socket(conn, true, &in_prog);
	if (conn->sock < 0)
	{
		pg_log_error("%s", conn->err_buf.data);
		active_driver->finish(conn);
		return NULL;
	}
	conn->conn_status = in_prog ? PGBENCH_CONN_POLLING_WRITING : PGBENCH_CONN_OK;
	return conn;
}

static PgBenchConnStatus
quack_connect_poll(PgBenchConn *conn)
{
	int			so_err = 0;
	socklen_t	slen = sizeof(so_err);

	if (conn == NULL || conn->sock < 0)
		return PGBENCH_CONN_POLLING_FAILED;

	if (conn->conn_status == PGBENCH_CONN_OK)
		return PGBENCH_CONN_POLLING_OK;

	if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0)
		so_err = errno;

	if (so_err != 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "async connect to Quack server failed: %s\n",
						  strerror(so_err));
		conn->conn_status = PGBENCH_CONN_BAD;
		return PGBENCH_CONN_POLLING_FAILED;
	}

	conn->conn_status = PGBENCH_CONN_OK;
	return PGBENCH_CONN_POLLING_OK;
}

static void
quack_finish(PgBenchConn *conn)
{
	if (conn == NULL)
		return;

	if (conn->sock >= 0)
		close(conn->sock);

	while (conn->op_head != NULL)
		pg_free(quack_pop_op(conn));

	while (conn->res_head != NULL)
	{
		PgBenchResult *r = conn->res_head;

		conn->res_head = r->next;
		quack_free_result(r);
	}

	while (conn->prep_head != NULL)
	{
		QuackPreparedStmt *ps = conn->prep_head;

		conn->prep_head = ps->next;
		pg_free(ps->name);
		pg_free(ps->sql);
		pg_free(ps);
	}

	termPQExpBuffer(&conn->err_buf);
	pg_free(conn->host);
	pg_free(conn->port);
	pg_free(conn->user);
	pg_free(conn->dbname);
	pg_free(conn->endpoint_path);
	pg_free(conn->auth_token);
	pg_free(conn->in_buf);
	pg_free(conn->out_buf);
	pg_free(conn);
}

static PgBenchConnStatus
quack_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_CONN_BAD;
	return (conn->conn_status == PGBENCH_CONN_OK ||
			conn->conn_status == PGBENCH_CONN_POLLING_WRITING ||
			conn->conn_status == PGBENCH_CONN_POLLING_READING) ?
		PGBENCH_CONN_OK : PGBENCH_CONN_BAD;
}

static const char *
quack_error_message(const PgBenchConn *conn)
{
	if (conn == NULL)
		return "connection is NULL";
	return conn->err_buf.data ? conn->err_buf.data : "";
}

static int
quack_get_poll_fd(const PgBenchConn *conn)
{
	if (conn == NULL || conn->sock < 0)
		return PGBENCH_POLL_ERROR;
	return conn->sock;
}

static PgBenchTxStatus
quack_tx_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_TX_UNKNOWN;
	return conn->tx_status;
}

static const char *
quack_get_host(const PgBenchConn *conn)
{
	return (conn && conn->host) ? conn->host : "";
}

static const char *
quack_get_port(const PgBenchConn *conn)
{
	return (conn && conn->port) ? conn->port : "";
}

static const char *
quack_get_db(const PgBenchConn *conn)
{
	return (conn && conn->dbname) ? conn->dbname : "";
}

static void
quack_set_cancel_conn(PgBenchConn *conn)
{
	(void) conn;
}

static void
quack_reset_cancel_conn(void)
{
}

static bool
quack_consume_input(PgBenchConn *conn)
{
	if (conn == NULL || conn->sock < 0)
		return false;

	if (!quack_flush_out(conn))
		return false;

	for (;;)
	{
		ssize_t		n;

		if (conn->in_len + 4096 > conn->in_cap)
		{
			conn->in_cap *= 2;
			conn->in_buf = pg_realloc(conn->in_buf, conn->in_cap);
		}

		n = recv(conn->sock, conn->in_buf + conn->in_len,
				 conn->in_cap - conn->in_len, 0);
		if (n > 0)
		{
			conn->in_len += (size_t) n;
			continue;
		}
		if (n == 0)
		{
			/* Server closed connection */
			if (conn->op_head != NULL && conn->in_len == 0)
			{
				printfPQExpBuffer(&conn->err_buf,
								  "Quack server closed connection unexpectedly\n");
				close(conn->sock);
				conn->sock = -1;
				return false;
			}
			close(conn->sock);
			conn->sock = -1;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;

		printfPQExpBuffer(&conn->err_buf,
						  "Quack socket recv failed: %s\n", strerror(errno));
		return false;
	}

	return quack_try_parse_http_responses(conn);
}

static bool
quack_is_busy(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	quack_try_parse_http_responses(conn);
	if (conn->res_head != NULL)
		return false;
	if (conn->awaiting_result_null)
		return false;
	return conn->op_head != NULL;
}

static PgBenchResult *
quack_get_result(PgBenchConn *conn)
{
	if (conn == NULL)
		return NULL;

	/* Return NULL separator between completed query cycles */
	if (conn->awaiting_result_null)
	{
		conn->awaiting_result_null = false;
		return NULL;
	}

	quack_try_parse_http_responses(conn);

	/* Block until result arrives if still in flight (e.g. during synchronous calls) */
	while (conn->res_head == NULL && conn->op_head != NULL)
	{
		fd_set		rfds;
		fd_set		wfds;

		if (conn->sock < 0)
			return NULL;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(conn->sock, &rfds);
		if (conn->out_sent < conn->out_len)
			FD_SET(conn->sock, &wfds);

		if (select(conn->sock + 1, &rfds,
				   (conn->out_sent < conn->out_len) ? &wfds : NULL,
				   NULL, NULL) < 0)
		{
			if (errno == EINTR)
				continue;
			return NULL;
		}
		if (!quack_consume_input(conn))
			return NULL;
	}

	if (conn->res_head != NULL)
	{
		PgBenchResult *res = conn->res_head;

		conn->res_head = res->next;
		if (conn->res_head == NULL)
			conn->res_tail = NULL;
		res->next = NULL;

		if (res->status != PGBENCH_RES_PIPELINE_SYNC)
			conn->awaiting_result_null = true;
		return res;
	}

	return NULL;
}

static PgBenchResult *
quack_exec_sync(PgBenchConn *conn, const char *sql)
{
	PgBenchResult *res;
	PgBenchResult *tmp;

	if (!quack_queue_http_query(conn, sql))
		return quack_make_result(PGBENCH_RES_FATAL_ERROR,
								 conn->err_buf.data, "08006");

	res = quack_get_result(conn);
	while ((tmp = quack_get_result(conn)) != NULL)
	{
		quack_free_result(res);
		res = tmp;
	}
	return res;
}

static void
quack_register_prepared(PgBenchConn *conn, const char *stmt_name,
						const char *sql, int nparams)
{
	QuackPreparedStmt *ps;

	for (ps = conn->prep_head; ps != NULL; ps = ps->next)
	{
		if (strcmp(ps->name, stmt_name) == 0)
		{
			pg_free(ps->sql);
			ps->sql = pg_strdup(sql);
			ps->nparams = nparams;
			return;
		}
	}
	ps = pg_malloc0_object(QuackPreparedStmt);
	ps->name = pg_strdup(stmt_name);
	ps->sql = pg_strdup(sql);
	ps->nparams = nparams;
	ps->next = conn->prep_head;
	conn->prep_head = ps;
}

static QuackPreparedStmt *
quack_find_prepared(PgBenchConn *conn, const char *stmt_name)
{
	for (QuackPreparedStmt *ps = conn->prep_head; ps != NULL; ps = ps->next)
	{
		if (strcmp(ps->name, stmt_name) == 0)
			return ps;
	}
	return NULL;
}

static PgBenchResult *
quack_prepare_sync(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	quack_register_prepared(conn, stmt_name, sql, nparams);
	return quack_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
}

static char *
quack_escape_identifier(PgBenchConn *conn, const char *str, size_t len)
{
	PQExpBufferData buf;

	(void) conn;
	initPQExpBuffer(&buf);
	appendPQExpBufferChar(&buf, '"');
	for (size_t i = 0; i < len; i++)
	{
		if (str[i] == '"')
			appendPQExpBufferStr(&buf, "\"\"");
		else
			appendPQExpBufferChar(&buf, str[i]);
	}
	appendPQExpBufferChar(&buf, '"');
	return buf.data;
}

static void
quack_free_mem(void *ptr)
{
	pg_free(ptr);
}

static bool
quack_send_query(PgBenchConn *conn, const char *sql)
{
	return quack_queue_http_query(conn, sql);
}

static bool
quack_send_query_params(PgBenchConn *conn, const char *sql, int nparams,
						const char *const *param_texts,
						const struct Variable *const *param_vars)
{
	char	   *bound = quack_bind_sql_params(sql, nparams, param_texts, param_vars);
	bool		ok = quack_queue_http_query(conn, bound);

	pg_free(bound);
	return ok;
}

static bool
quack_send_prepare(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	quack_register_prepared(conn, stmt_name, sql, nparams);
	quack_enqueue_op(conn, QUACK_PENDING_SYNTHETIC_OK, 0);
	quack_drain_non_http_ops(conn);
	return true;
}

static bool
quack_send_query_prepared(PgBenchConn *conn, const char *stmt_name, int nparams,
						  const char *const *param_texts,
						  const struct Variable *const *param_vars)
{
	QuackPreparedStmt *ps = quack_find_prepared(conn, stmt_name);

	if (ps == NULL)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "prepared statement \"%s\" does not exist\n",
						  stmt_name ? stmt_name : "");
		return false;
	}
	return quack_send_query_params(conn, ps->sql, nparams, param_texts, param_vars);
}

static bool
quack_enter_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	conn->pipe_status = PGBENCH_PIPE_ON;
	conn->pipe_aborted = false;
	conn->awaiting_result_null = false;
	return true;
}

static bool
quack_exit_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	if (conn->op_head != NULL || conn->res_head != NULL)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "cannot exit pipeline mode with uncollected results\n");
		return false;
	}
	conn->pipe_status = PGBENCH_PIPE_OFF;
	conn->pipe_aborted = false;
	conn->awaiting_result_null = false;
	return true;
}

static PgBenchPipelineStatus
quack_pipeline_status(const PgBenchConn *conn)
{
	return conn ? conn->pipe_status : PGBENCH_PIPE_OFF;
}

static bool
quack_send_pipeline_sync(PgBenchConn *conn)
{
	if (conn == NULL || conn->pipe_status == PGBENCH_PIPE_OFF)
		return false;
	quack_enqueue_op(conn, QUACK_PENDING_PIPELINE_SYNC, 0);
	quack_drain_non_http_ops(conn);
	return quack_flush_out(conn);
}

static bool
quack_pipeline_sync(PgBenchConn *conn)
{
	return quack_send_pipeline_sync(conn);
}

static PgBenchExecStatus
quack_result_status(const PgBenchResult *res)
{
	return res ? res->status : PGBENCH_RES_FATAL_ERROR;
}

static const char *
quack_result_error_field(const PgBenchResult *res, int fieldcode)
{
	if (res == NULL)
		return NULL;
	if (fieldcode == PG_DIAG_SQLSTATE && res->sqlstate[0] != '\0')
		return res->sqlstate;
	return NULL;
}

static const char *
quack_result_error_message(const PgBenchResult *res, const PgBenchConn *conn)
{
	if (res != NULL && res->err_msg != NULL && *res->err_msg != '\0')
		return res->err_msg;
	return quack_error_message(conn);
}

static int
quack_ntuples(const PgBenchResult *res)
{
	return res ? res->nrows : 0;
}

static int
quack_nfields(const PgBenchResult *res)
{
	return res ? res->ncols : 0;
}

static const char *
quack_fname(const PgBenchResult *res, int col)
{
	if (res == NULL || col < 0 || col >= res->ncols || res->col_names == NULL)
		return NULL;
	return res->col_names[col];
}

static int
quack_fnumber(const PgBenchResult *res, const char *col_name)
{
	if (res == NULL || col_name == NULL || res->col_names == NULL)
		return -1;
	for (int c = 0; c < res->ncols; c++)
	{
		if (res->col_names[c] != NULL &&
			pg_strcasecmp(res->col_names[c], col_name) == 0)
			return c;
	}
	return -1;
}

static bool
quack_get_is_null(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return true;
	return res->cells[row][col] == NULL;
}

static const char *
quack_get_value(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return "";
	return res->cells[row][col] ? res->cells[row][col] : "";
}

static void
quack_clear_result(PgBenchResult *res)
{
	quack_free_result(res);
}

/* -------------------------------------------------------------------------
 * Bulk Load (COPY / Batched Multi-Row INSERT)
 * -------------------------------------------------------------------------
 */

static bool
quack_flush_copy_batch(PgBenchCopyStream *stream)
{
	PgBenchResult *res;
	bool		ok;

	if (stream->batch_rows == 0)
		return true;

	res = quack_exec_sync(stream->conn, stream->batch_sql.data);
	ok = (res != NULL &&
		  (res->status == PGBENCH_RES_COMMAND_OK ||
		   res->status == PGBENCH_RES_TUPLES_OK));
	quack_free_result(res);
	resetPQExpBuffer(&stream->batch_sql);
	stream->batch_rows = 0;
	if (!ok)
		stream->failed = true;
	return ok;
}

static void
quack_begin_batch_sql_if_needed(PgBenchCopyStream *stream)
{
	if (stream->batch_rows == 0)
	{
		printfPQExpBuffer(&stream->batch_sql, "INSERT INTO %s", stream->table_name);
		if (stream->ncols > 0 && stream->col_names != NULL)
		{
			appendPQExpBufferChar(&stream->batch_sql, '(');
			for (int i = 0; i < stream->ncols; i++)
			{
				if (i > 0)
					appendPQExpBufferStr(&stream->batch_sql, ", ");
				appendPQExpBufferStr(&stream->batch_sql, stream->col_names[i]);
			}
			appendPQExpBufferChar(&stream->batch_sql, ')');
		}
		appendPQExpBufferStr(&stream->batch_sql, " VALUES ");
	}
	else
	{
		appendPQExpBufferStr(&stream->batch_sql, ", ");
	}
}

static bool
quack_append_tsv_line_to_batch(PgBenchCopyStream *stream, const char *line, size_t len)
{
	const char *p = line;
	const char *end = line + len;
	int			cidx = 0;

	if (len > 0 && end[-1] == '\r')
		end--;
	if (p == end)
		return true;

	quack_begin_batch_sql_if_needed(stream);
	appendPQExpBufferChar(&stream->batch_sql, '(');

	for (const char *c = p; c <= end; c++)
	{
		if (c == end || *c == '\t')
		{
			size_t		clen = c - p;

			if (cidx > 0)
				appendPQExpBufferStr(&stream->batch_sql, ", ");

			if (clen == 2 && p[0] == '\\' && p[1] == 'N')
			{
				appendPQExpBufferStr(&stream->batch_sql, "NULL");
			}
			else
			{
				char	   *val = pnstrdup(p, clen);

				quack_append_param_literal(&stream->batch_sql, val, NULL);
				pg_free(val);
			}
			cidx++;
			p = c + 1;
		}
	}
	appendPQExpBufferChar(&stream->batch_sql, ')');
	stream->batch_rows++;

	if (stream->batch_rows >= 500 || stream->batch_sql.len >= 262144)
		return quack_flush_copy_batch(stream);
	return true;
}

static PgBenchCopyStream *
quack_copy_begin(PgBenchConn *conn, const char *table_name,
				 int ncols, const char *const *col_names,
				 const char *copy_sql)
{
	PgBenchCopyStream *stream = pg_malloc0_object(PgBenchCopyStream);

	(void) copy_sql;
	stream->conn = conn;
	stream->table_name = pg_strdup(table_name ? table_name : "pgbench_accounts");
	stream->ncols = ncols;
	if (ncols > 0 && col_names != NULL)
	{
		stream->col_names = pg_malloc0_array(char *, ncols);
		for (int i = 0; i < ncols; i++)
			stream->col_names[i] = pg_strdup(col_names[i]);
	}
	initPQExpBuffer(&stream->line_buf);
	initPQExpBuffer(&stream->batch_sql);
	return stream;
}

static bool
quack_copy_put_data(PgBenchCopyStream *stream, const char *buffer, int nbytes)
{
	const char *p;
	const char *end;

	if (stream == NULL || stream->failed)
		return false;

	appendBinaryPQExpBuffer(&stream->line_buf, buffer, nbytes);
	p = stream->line_buf.data;
	end = p + stream->line_buf.len;

	for (;;)
	{
		const char *nl = memchr(p, '\n', end - p);

		if (nl == NULL)
			break;
		if (!quack_append_tsv_line_to_batch(stream, p, nl - p))
			return false;
		p = nl + 1;
	}

	if (p > stream->line_buf.data)
	{
		size_t		rem = end - p;

		if (rem > 0)
			memmove(stream->line_buf.data, p, rem);
		stream->line_buf.len = rem;
		stream->line_buf.data[rem] = '\0';
	}
	return true;
}

static bool
quack_copy_put_row(PgBenchCopyStream *stream, int ncols,
				   const struct PgBenchValue *cols)
{
	if (stream == NULL || stream->failed)
		return false;

	quack_begin_batch_sql_if_needed(stream);
	appendPQExpBufferChar(&stream->batch_sql, '(');
	for (int i = 0; i < ncols; i++)
	{
		struct Variable tmp_var;

		if (i > 0)
			appendPQExpBufferStr(&stream->batch_sql, ", ");
		memset(&tmp_var, 0, sizeof(tmp_var));
		tmp_var.value = cols[i];
		quack_append_param_literal(&stream->batch_sql,
								   (cols[i].type == PGBT_TEXT) ? cols[i].u.sval : NULL,
								   &tmp_var);
	}
	appendPQExpBufferChar(&stream->batch_sql, ')');
	stream->batch_rows++;

	if (stream->batch_rows >= 500 || stream->batch_sql.len >= 262144)
		return quack_flush_copy_batch(stream);
	return true;
}

static bool
quack_copy_end(PgBenchCopyStream *stream, const char *errormsg)
{
	bool		ok = true;

	if (stream == NULL)
		return false;

	if (errormsg == NULL && !stream->failed)
	{
		if (stream->line_buf.len > 0)
			ok = quack_append_tsv_line_to_batch(stream,
												stream->line_buf.data,
												stream->line_buf.len);
		if (ok)
			ok = quack_flush_copy_batch(stream);
	}
	else
		ok = false;

	termPQExpBuffer(&stream->line_buf);
	termPQExpBuffer(&stream->batch_sql);
	pg_free(stream->table_name);
	if (stream->col_names != NULL)
	{
		for (int i = 0; i < stream->ncols; i++)
			pg_free(stream->col_names[i]);
		pg_free(stream->col_names);
	}
	pg_free(stream);
	return ok;
}

const PgBenchDbDriver quack_driver = {
	.name = "quack",
	.caps = {
		.supports_pipeline = true,
		.supports_async_connect = true,
		.supports_vacuum = false,
		.supports_pg_catalog = false,
		.supports_copy_text = true,
		.supports_typed_copy = true,
		.prefer_prepared_mode = false,
		.param_style = PGBENCH_PARAM_DOLLAR
	},
	.init_global = quack_init_global,
	.shutdown_global = quack_shutdown_global,
	.connect_sync = quack_connect_sync,
	.connect_start = quack_connect_start,
	.connect_poll = quack_connect_poll,
	.finish = quack_finish,
	.status = quack_status,
	.error_message = quack_error_message,
	.get_poll_fd = quack_get_poll_fd,
	.tx_status = quack_tx_status,
	.get_host = quack_get_host,
	.get_port = quack_get_port,
	.get_db = quack_get_db,
	.set_cancel_conn = quack_set_cancel_conn,
	.reset_cancel_conn = quack_reset_cancel_conn,
	.exec_sync = quack_exec_sync,
	.prepare_sync = quack_prepare_sync,
	.escape_identifier = quack_escape_identifier,
	.free_mem = quack_free_mem,
	.send_query = quack_send_query,
	.send_query_params = quack_send_query_params,
	.send_prepare = quack_send_prepare,
	.send_query_prepared = quack_send_query_prepared,
	.consume_input = quack_consume_input,
	.is_busy = quack_is_busy,
	.get_result = quack_get_result,
	.enter_pipeline_mode = quack_enter_pipeline_mode,
	.exit_pipeline_mode = quack_exit_pipeline_mode,
	.pipeline_status = quack_pipeline_status,
	.pipeline_sync = quack_pipeline_sync,
	.send_pipeline_sync = quack_send_pipeline_sync,
	.result_status = quack_result_status,
	.result_error_field = quack_result_error_field,
	.result_error_message = quack_result_error_message,
	.ntuples = quack_ntuples,
	.nfields = quack_nfields,
	.fname = quack_fname,
	.fnumber = quack_fnumber,
	.get_is_null = quack_get_is_null,
	.get_value = quack_get_value,
	.clear_result = quack_clear_result,
	.copy_begin = quack_copy_begin,
	.copy_put_data = quack_copy_put_data,
	.copy_put_row = quack_copy_put_row,
	.copy_end = quack_copy_end
};
