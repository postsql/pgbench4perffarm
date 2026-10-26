/*-------------------------------------------------------------------------
 *
 * spanner_driver.c
 *		Cloud Spanner (v1 HTTP/JSON REST + eventfd async worker)
 *		implementation of the PgBenchDbDriver vtable
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/spanner_driver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#ifdef USE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include "common/logging.h"
#include "common/string.h"
#include "db_driver.h"
#include "engine.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "variable.h"

#define SPANNER_TOKEN_TTL_SEC 2700	/* Refresh OAuth2 token every 45 minutes */

static pthread_mutex_t spanner_token_mu = PTHREAD_MUTEX_INITIALIZER;
static char *spanner_cached_token = NULL;
static time_t spanner_token_fetched_at = 0;

typedef struct SpannerPreparedStmt
{
	char	   *name;
	char	   *sql;
	int			nparams;
	struct SpannerPreparedStmt *next;
} SpannerPreparedStmt;

typedef enum SpannerTaskKind
{
	SPANNER_TASK_CONNECT = 0,
	SPANNER_TASK_QUERY,
	SPANNER_TASK_PREPARE,
	SPANNER_TASK_PIPELINE_SYNC,
	SPANNER_TASK_BATCH_DML
} SpannerTaskKind;

typedef struct SpannerBatchStmt
{
	char	   *sql;
	char	   *params_json;
	char	   *param_types_json;
} SpannerBatchStmt;

typedef struct SpannerTask
{
	SpannerTaskKind kind;
	char	   *sql;
	char	   *params_json;
	char	   *param_types_json;
	SpannerBatchStmt *batch_stmts;
	int			num_batch_stmts;
	struct SpannerTask *next;
} SpannerTask;

struct PgBenchResult
{
	PgBenchExecStatus status;
	char	   *err_msg;
	char		sqlstate[6];
	int			nrows;
	int			ncols;
	char	  **col_names;
	char	 ***cells;
	struct PgBenchResult *next;
};

struct PgBenchConn
{
	char	   *host;
	char	   *port;
	char	   *project;
	char	   *instance;
	char	   *database;
	char	   *db_resource;	/* projects/{p}/instances/{i}/databases/{d} */
	char	   *session_name;	/* projects/.../sessions/{sid} */
	char	   *auth_token;
	bool		is_pg_dialect;
	bool		use_tls;
#ifdef USE_OPENSSL
	SSL_CTX    *ssl_ctx;
	SSL		   *ssl;
#endif

	int			http_sock;
	int			wakeup_fd;		/* eventfd or pipe read end for poller.c */
	int			wakeup_write_fd;	/* -1 when using eventfd, pipe[1] otherwise */

	PgBenchConnStatus conn_status;
	PgBenchTxStatus tx_status;
	PgBenchPipelineStatus pipe_status;
	bool		pipe_aborted;

	char	   *active_tx_id;
	int64		tx_seqno;
	char	   *precommit_token;
	int64		precommit_seq;

	PQExpBufferData err_buf;

	/* Buffered DML statements during pipeline mode */
	SpannerBatchStmt *pipe_batch;
	int			pipe_batch_count;
	int			pipe_batch_cap;

	/* Background worker synchronization */
	pthread_mutex_t mu;
	pthread_cond_t cv;
	pthread_t	worker_tid;
	bool		worker_running;
	bool		worker_stop;
	int			tasks_in_flight;

	SpannerTask *task_head;
	SpannerTask *task_tail;

	PgBenchResult *res_head;
	PgBenchResult *res_tail;
	bool		awaiting_result_null;

	SpannerPreparedStmt *prep_head;
};

struct PgBenchCopyStream
{
	PgBenchConn *conn;
	char	   *table_name;
	int			ncols;
	char	  **col_names;
	PQExpBufferData line_buf;
	PQExpBufferData values_json;
	int			batch_rows;
	bool		failed;
};

/* -------------------------------------------------------------------------
 * Result & JSON Helpers
 * -------------------------------------------------------------------------
 */

static PgBenchResult *
spanner_make_result(PgBenchExecStatus status, const char *err_msg,
					const char *sqlstate)
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
spanner_free_result(PgBenchResult *res)
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
spanner_append_json_escaped(PQExpBuffer buf, const char *s)
{
	appendPQExpBufferChar(buf, '"');
	if (s != NULL)
	{
		for (const char *p = s; *p != '\0'; p++)
		{
			unsigned char ch = (unsigned char) *p;

			switch (ch)
			{
				case '"':
					appendPQExpBufferStr(buf, "\\\"");
					break;
				case '\\':
					appendPQExpBufferStr(buf, "\\\\");
					break;
				case '\b':
					appendPQExpBufferStr(buf, "\\b");
					break;
				case '\f':
					appendPQExpBufferStr(buf, "\\f");
					break;
				case '\n':
					appendPQExpBufferStr(buf, "\\n");
					break;
				case '\r':
					appendPQExpBufferStr(buf, "\\r");
					break;
				case '\t':
					appendPQExpBufferStr(buf, "\\t");
					break;
				default:
					if (ch < 0x20)
						appendPQExpBuffer(buf, "\\u%04x", (unsigned int) ch);
					else
						appendPQExpBufferChar(buf, (char) ch);
					break;
			}
		}
	}
	appendPQExpBufferChar(buf, '"');
}

static bool
spanner_is_int64_str(const char *s)
{
	const char *p = s;

	if (p == NULL || *p == '\0')
		return false;
	if (*p == '-' || *p == '+')
		p++;
	if (*p == '\0')
		return false;
	if (p[0] == '0' && p[1] != '\0')
		return false;
	while (*p != '\0')
	{
		if (!isdigit((unsigned char) *p))
			return false;
		p++;
	}
	return strlen(s) <= 19;
}

static bool
spanner_is_float64_str(const char *s, double *out_val)
{
	const char *p = s;
	bool		has_dot_or_exp = false;
	bool		has_digit = false;
	char	   *endp = NULL;
	double		d;

	if (p == NULL || *p == '\0')
		return false;
	if (*p == '-' || *p == '+')
		p++;
	for (; *p != '\0'; p++)
	{
		if (isdigit((unsigned char) *p))
			has_digit = true;
		else if (*p == '.' || *p == 'e' || *p == 'E')
			has_dot_or_exp = true;
		else if (*p != '-' && *p != '+')
			return false;
	}
	if (!has_digit || !has_dot_or_exp)
		return false;

	errno = 0;
	d = strtod(s, &endp);
	if (errno != 0 || endp == s || *endp != '\0' || !isfinite(d))
		return false;
	if (out_val != NULL)
		*out_val = d;
	return true;
}

static void
spanner_build_params_json(int nparams,
						  const char *const *param_texts,
						  const struct Variable *const *param_vars,
						  char **out_params_json,
						  char **out_types_json)
{
	PQExpBufferData pbuf;
	PQExpBufferData tbuf;

	if (nparams <= 0)
	{
		*out_params_json = NULL;
		*out_types_json = NULL;
		return;
	}

	initPQExpBuffer(&pbuf);
	initPQExpBuffer(&tbuf);
	appendPQExpBufferChar(&pbuf, '{');
	appendPQExpBufferChar(&tbuf, '{');

	for (int i = 0; i < nparams; i++)
	{
		const char *tval = param_texts ? param_texts[i] : NULL;
		const struct Variable *v = param_vars ? param_vars[i] : NULL;
		double		dval = 0.0;

		if (i > 0)
		{
			appendPQExpBufferStr(&pbuf, ", ");
			appendPQExpBufferStr(&tbuf, ", ");
		}

		appendPQExpBuffer(&pbuf, "\"p%d\": ", i + 1);
		appendPQExpBuffer(&tbuf, "\"p%d\": ", i + 1);

		if (v != NULL && v->value.type != PGBT_NO_VALUE)
		{
			switch (v->value.type)
			{
				case PGBT_NULL:
					appendPQExpBufferStr(&pbuf, "null");
					appendPQExpBufferStr(&tbuf, "{\"code\": \"STRING\"}");
					continue;
				case PGBT_INT:
					appendPQExpBuffer(&pbuf, "\"" INT64_FORMAT "\"", v->value.u.ival);
					appendPQExpBufferStr(&tbuf, "{\"code\": \"INT64\"}");
					continue;
				case PGBT_DOUBLE:
					appendPQExpBuffer(&pbuf, "%.17g", v->value.u.dval);
					appendPQExpBufferStr(&tbuf, "{\"code\": \"FLOAT64\"}");
					continue;
				case PGBT_BOOLEAN:
					appendPQExpBufferStr(&pbuf, v->value.u.bval ? "true" : "false");
					appendPQExpBufferStr(&tbuf, "{\"code\": \"BOOL\"}");
					continue;
				default:
					break;
			}
		}

		if (tval == NULL)
		{
			appendPQExpBufferStr(&pbuf, "null");
			appendPQExpBufferStr(&tbuf, "{\"code\": \"STRING\"}");
		}
		else if (spanner_is_int64_str(tval))
		{
			spanner_append_json_escaped(&pbuf, tval);
			appendPQExpBufferStr(&tbuf, "{\"code\": \"INT64\"}");
		}
		else if (pg_strcasecmp(tval, "true") == 0 ||
				 pg_strcasecmp(tval, "false") == 0)
		{
			appendPQExpBufferStr(&pbuf,
								 (pg_strcasecmp(tval, "true") == 0) ? "true" : "false");
			appendPQExpBufferStr(&tbuf, "{\"code\": \"BOOL\"}");
		}
		else if (spanner_is_float64_str(tval, &dval))
		{
			appendPQExpBuffer(&pbuf, "%.17g", dval);
			appendPQExpBufferStr(&tbuf, "{\"code\": \"FLOAT64\"}");
		}
		else
		{
			spanner_append_json_escaped(&pbuf, tval);
			appendPQExpBufferStr(&tbuf, "{\"code\": \"STRING\"}");
		}
	}

	appendPQExpBufferChar(&pbuf, '}');
	appendPQExpBufferChar(&tbuf, '}');
	*out_params_json = pbuf.data;
	*out_types_json = tbuf.data;
}

/* -------------------------------------------------------------------------
 * Lightweight JSON Parser for Spanner v1 REST Responses
 * -------------------------------------------------------------------------
 */

static const char *
sp_json_skip_ws(const char *p, const char *end)
{
	while (p < end && isspace((unsigned char) *p))
		p++;
	return p;
}

static char *
sp_json_parse_string(const char **pp, const char *end)
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
sp_json_skip_value(const char *p, const char *end)
{
	p = sp_json_skip_ws(p, end);
	if (p >= end)
		return end;

	if (*p == '"')
	{
		char	   *s = sp_json_parse_string(&p, end);

		pg_free(s);
		return p;
	}
	if (*p == '{' || *p == '[')
	{
		char		open_ch = *p;
		char		close_ch = (open_ch == '{') ? '}' : ']';
		int			depth = 1;

		p++;
		while (p < end && depth > 0)
		{
			if (*p == '"')
			{
				char	   *s = sp_json_parse_string(&p, end);

				pg_free(s);
				continue;
			}
			if (*p == open_ch)
				depth++;
			else if (*p == close_ch)
				depth--;
			p++;
		}
		return p;
	}
	while (p < end && *p != ',' && *p != '}' && *p != ']' &&
		   !isspace((unsigned char) *p))
		p++;
	return p;
}

static const char *
sp_json_find_field(const char *obj, const char *end, const char *key)
{
	size_t		klen = strlen(key);
	const char *p = sp_json_skip_ws(obj, end);

	if (p >= end || *p != '{')
		return NULL;
	p = sp_json_skip_ws(p + 1, end);

	while (p < end && *p != '}')
	{
		if (*p == '"')
		{
			const char *kstart = p + 1;
			char	   *kname = sp_json_parse_string(&p, end);
			bool		match = (kname != NULL && strlen(kname) == klen &&
								 memcmp(kname, key, klen) == 0);

			(void) kstart;
			pg_free(kname);
			p = sp_json_skip_ws(p, end);
			if (p < end && *p == ':')
			{
				p = sp_json_skip_ws(p + 1, end);
				if (match)
					return p;
				p = sp_json_skip_value(p, end);
			}
		}
		else
			p++;
		p = sp_json_skip_ws(p, end);
		if (p < end && *p == ',')
			p = sp_json_skip_ws(p + 1, end);
	}
	return NULL;
}

static char *
sp_json_extract_string_field(const char *obj, const char *end, const char *key)
{
	const char *v = sp_json_find_field(obj, end, key);

	if (v != NULL && v < end && *v == '"')
		return sp_json_parse_string(&v, end);
	return NULL;
}

/* -------------------------------------------------------------------------
 * OAuth2 Token Refresh & URI / Endpoint Parsing
 * -------------------------------------------------------------------------
 */

static void
spanner_refresh_bearer_token(PgBenchConn *conn, bool force_refresh)
{
	const char *cmd = getenv("SPANNER_TOKEN_COMMAND");
	const char *env_tok = getenv("SPANNER_API_TOKEN");

	if ((cmd == NULL || *cmd == '\0') && !force_refresh &&
		env_tok != NULL && *env_tok != '\0')
	{
		if (conn->auth_token == NULL)
			conn->auth_token = pg_strdup(env_tok);
		return;
	}

	if ((cmd == NULL || *cmd == '\0') && conn->use_tls &&
		getenv("SPANNER_EMULATOR_HOST") == NULL)
	{
		cmd = "gcloud auth application-default print-access-token 2>/dev/null || gcloud auth print-access-token 2>/dev/null";
	}

	if (cmd != NULL && *cmd != '\0')
	{
		time_t		now = time(NULL);

		pthread_mutex_lock(&spanner_token_mu);
		if (force_refresh || spanner_cached_token == NULL ||
			(now - spanner_token_fetched_at) >= SPANNER_TOKEN_TTL_SEC)
		{
			FILE	   *fp = popen(cmd, "r");

			if (fp != NULL)
			{
				char		tbuf[4096];
				size_t		nread = fread(tbuf, 1, sizeof(tbuf) - 1, fp);

				(void) pclose(fp);
				tbuf[nread] = '\0';
				while (nread > 0 &&
					   (tbuf[nread - 1] == '\n' || tbuf[nread - 1] == '\r' ||
						isspace((unsigned char) tbuf[nread - 1])))
				{
					tbuf[--nread] = '\0';
				}
				if (nread > 0)
				{
					pg_free(spanner_cached_token);
					spanner_cached_token = pg_strdup(tbuf);
					spanner_token_fetched_at = now;
				}
			}
		}
		if (spanner_cached_token != NULL)
		{
			if (conn->auth_token == NULL ||
				strcmp(conn->auth_token, spanner_cached_token) != 0)
			{
				pg_free(conn->auth_token);
				conn->auth_token = pg_strdup(spanner_cached_token);
			}
		}
		pthread_mutex_unlock(&spanner_token_mu);
		return;
	}

	if (conn->auth_token == NULL && env_tok != NULL && *env_tok != '\0')
		conn->auth_token = pg_strdup(env_tok);
}

static void
spanner_parse_conn_params(PgBenchConn *conn, const char *host, const char *port,
						  const char *user, const char *dbname)
{
	const char *env_emu = getenv("SPANNER_EMULATOR_HOST");
	const char *eff_db = (dbname && *dbname) ? dbname : "projects/test-project/instances/test-instance/databases/postgres";
	const char *p = eff_db;
	const char *qmark;
	bool		has_tls_override = false;

	(void) user;
	conn->project = pg_strdup("test-project");
	conn->instance = pg_strdup("test-instance");
	conn->database = pg_strdup("postgres");
	conn->is_pg_dialect = (driver_dialect != NULL &&
						   pg_strcasecmp(driver_dialect, "postgresql") == 0);

	if (pg_strncasecmp(p, "spanner://", 10) == 0)
		p += 10;
	if (*p == '/')
		p++;

	qmark = strchr(p, '?');
	if (qmark != NULL)
	{
		const char *ep = strstr(qmark + 1, "endpoint=");
		const char *hp = strstr(qmark + 1, "host=");
		const char *pp = strstr(qmark + 1, "port=");
		const char *dp = strstr(qmark + 1, "dialect=");
		const char *tp = strstr(qmark + 1, "tls=");
		const char *e_start = NULL;

		if (ep != NULL)
			e_start = ep + 9;
		else if (hp != NULL)
			e_start = hp + 5;

		if (e_start != NULL)
		{
			const char *e_end = strchr(e_start, '&');
			char	   *ep_str = e_end ?
				pnstrdup(e_start, e_end - e_start) : pg_strdup(e_start);
			char	   *ep_host = ep_str;
			char	   *colon;

			if (pg_strncasecmp(ep_host, "https://", 8) == 0)
			{
				ep_host += 8;
				conn->use_tls = true;
				has_tls_override = true;
			}
			else if (pg_strncasecmp(ep_host, "http://", 7) == 0)
			{
				ep_host += 7;
				conn->use_tls = false;
				has_tls_override = true;
			}

			colon = strchr(ep_host, ':');
			if (colon != NULL)
			{
				*colon = '\0';
				conn->host = pg_strdup(ep_host);
				conn->port = pg_strdup(colon + 1);
			}
			else
				conn->host = pg_strdup(ep_host);
			pg_free(ep_str);
		}
		if (pp != NULL && conn->port == NULL)
		{
			const char *p_start = pp + 5;
			const char *p_end = strchr(p_start, '&');

			conn->port = p_end ?
				pnstrdup(p_start, p_end - p_start) : pg_strdup(p_start);
		}
		if (dp != NULL)
		{
			const char *d_start = dp + 8;

			if (pg_strncasecmp(d_start, "postgresql", 10) == 0 ||
				pg_strncasecmp(d_start, "postgres", 8) == 0 ||
				pg_strncasecmp(d_start, "pg", 2) == 0)
				conn->is_pg_dialect = true;
			else if (pg_strncasecmp(d_start, "googlesql", 9) == 0)
				conn->is_pg_dialect = false;
		}
		if (tp != NULL)
		{
			const char *t_start = tp + 4;

			if (*t_start == '1' || pg_strncasecmp(t_start, "true", 4) == 0)
			{
				conn->use_tls = true;
				has_tls_override = true;
			}
			else if (*t_start == '0' || pg_strncasecmp(t_start, "false", 5) == 0)
			{
				conn->use_tls = false;
				has_tls_override = true;
			}
		}
	}

	{
		size_t		path_len = qmark ? (size_t) (qmark - p) : strlen(p);
		char	   *path_copy = pnstrdup(p, path_len);
		char	   *s1;

		if (strncmp(path_copy, "projects/", 9) == 0)
		{
			char	   *proj = path_copy + 9;
			char	   *inst_tag = strstr(proj, "/instances/");

			if (inst_tag != NULL)
			{
				char	   *inst = inst_tag + 11;
				char	   *db_tag = strstr(inst, "/databases/");

				*inst_tag = '\0';
				pg_free(conn->project);
				conn->project = pg_strdup(proj);

				if (db_tag != NULL)
				{
					*db_tag = '\0';
					pg_free(conn->instance);
					conn->instance = pg_strdup(inst);
					pg_free(conn->database);
					conn->database = pg_strdup(db_tag + 11);
				}
			}
		}
		else if ((s1 = strchr(path_copy, '/')) != NULL)
		{
			char	   *s2 = strchr(s1 + 1, '/');

			if (s2 != NULL)
			{
				*s1 = '\0';
				*s2 = '\0';
				pg_free(conn->project);
				conn->project = pg_strdup(path_copy);
				pg_free(conn->instance);
				conn->instance = pg_strdup(s1 + 1);
				pg_free(conn->database);
				conn->database = pg_strdup(s2 + 1);
			}
			else
			{
				*s1 = '\0';
				pg_free(conn->instance);
				conn->instance = pg_strdup(path_copy);
				pg_free(conn->database);
				conn->database = pg_strdup(s1 + 1);
			}
		}
		else if (*path_copy != '\0')
		{
			pg_free(conn->database);
			conn->database = pg_strdup(path_copy);
		}
		pg_free(path_copy);
	}

	if (conn->host == NULL)
	{
		if (host != NULL && *host != '\0')
			conn->host = pg_strdup(host);
		else if (env_emu != NULL && *env_emu != '\0')
		{
			const char *colon = strchr(env_emu, ':');

			if (colon != NULL)
			{
				conn->host = pnstrdup(env_emu, colon - env_emu);
				if (conn->port == NULL)
					conn->port = pg_strdup(colon + 1);
			}
			else
				conn->host = pg_strdup(env_emu);
		}
		else
		{
			conn->host = pg_strdup("spanner.googleapis.com");
			if (conn->port == NULL && (port == NULL || *port == '\0'))
				conn->port = pg_strdup("443");
			if (!has_tls_override)
				conn->use_tls = true;
		}
	}

	if (conn->port == NULL)
	{
		if (port != NULL && *port != '\0')
			conn->port = pg_strdup(port);
		else if (conn->use_tls ||
				 pg_strcasecmp(conn->host, "spanner.googleapis.com") == 0)
			conn->port = pg_strdup("443");
		else
			conn->port = pg_strdup("9020");
	}

	if (!has_tls_override)
	{
		conn->use_tls = (strcmp(conn->port, "443") == 0 ||
						 pg_strcasecmp(conn->host, "spanner.googleapis.com") == 0);
	}

	conn->db_resource = psprintf("projects/%s/instances/%s/databases/%s",
								 conn->project, conn->instance, conn->database);
	spanner_refresh_bearer_token(conn, false);
}

/* -------------------------------------------------------------------------
 * Synchronous HTTP/1.1 (with optional OpenSSL TLS) JSON RPC Client
 * -------------------------------------------------------------------------
 */

static void
spanner_disconnect_tcp(PgBenchConn *conn)
{
#ifdef USE_OPENSSL
	if (conn->ssl != NULL)
	{
		(void) SSL_shutdown(conn->ssl);
		SSL_free(conn->ssl);
		conn->ssl = NULL;
	}
#endif
	if (conn->http_sock >= 0)
	{
		close(conn->http_sock);
		conn->http_sock = -1;
	}
}

static int
spanner_connect_tcp(PgBenchConn *conn)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int			sock = -1;
	int			rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rc = getaddrinfo(conn->host, conn->port, &hints, &res);
	if (rc != 0 || res == NULL)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not resolve Spanner endpoint \"%s:%s\": %s\n",
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
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(sock);
		sock = -1;
	}

	freeaddrinfo(res);

	if (sock < 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not connect to Spanner endpoint at \"%s:%s\": %s\n",
						  conn->host, conn->port, strerror(errno));
		return -1;
	}

	if (conn->use_tls)
	{
#ifdef USE_OPENSSL
		if (conn->ssl_ctx == NULL)
		{
			conn->ssl_ctx = SSL_CTX_new(TLS_client_method());
			if (conn->ssl_ctx == NULL)
			{
				printfPQExpBuffer(&conn->err_buf,
								  "could not create OpenSSL SSL_CTX for \"%s:%s\"\n",
								  conn->host, conn->port);
				close(sock);
				return -1;
			}
		}
		conn->ssl = SSL_new(conn->ssl_ctx);
		if (conn->ssl == NULL)
		{
			printfPQExpBuffer(&conn->err_buf,
							  "could not create OpenSSL SSL object for \"%s:%s\"\n",
							  conn->host, conn->port);
			close(sock);
			return -1;
		}
		(void) SSL_set_tlsext_host_name(conn->ssl, conn->host);
		SSL_set_fd(conn->ssl, sock);
		if (SSL_connect(conn->ssl) != 1)
		{
			unsigned long ssl_err = ERR_get_error();

			printfPQExpBuffer(&conn->err_buf,
							  "TLS handshake with Spanner endpoint \"%s:%s\" failed: %s\n",
							  conn->host, conn->port,
							  ssl_err ? ERR_reason_error_string(ssl_err) : "SSL_connect error");
			SSL_free(conn->ssl);
			conn->ssl = NULL;
			close(sock);
			return -1;
		}
#else
		printfPQExpBuffer(&conn->err_buf,
						  "Spanner TLS connection required for \"%s:%s\", but pgbench was built without OpenSSL\n",
						  conn->host, conn->port);
		close(sock);
		return -1;
#endif
	}

	return sock;
}

static ssize_t
spanner_socket_write(PgBenchConn *conn, const char *buf, size_t len)
{
#ifdef USE_OPENSSL
	if (conn->ssl != NULL)
		return (ssize_t) SSL_write(conn->ssl, buf, (int) len);
#endif
#ifdef MSG_NOSIGNAL
	return send(conn->http_sock, buf, len, MSG_NOSIGNAL);
#else
	return send(conn->http_sock, buf, len, 0);
#endif
}

static ssize_t
spanner_socket_read(PgBenchConn *conn, char *buf, size_t cap)
{
#ifdef USE_OPENSSL
	if (conn->ssl != NULL)
		return (ssize_t) SSL_read(conn->ssl, buf, (int) cap);
#endif
	return recv(conn->http_sock, buf, cap, 0);
}

static bool
spanner_http_rpc(PgBenchConn *conn, const char *method, const char *uri_path,
				 const char *json_body, int *out_http_status,
				 char **out_resp_body, size_t *out_resp_len)
{
	PQExpBufferData req;
	size_t		body_len = json_body ? strlen(json_body) : 0;
	int			attempt;
	bool		retried_auth = false;

	*out_http_status = 500;
	*out_resp_body = NULL;
	*out_resp_len = 0;

	initPQExpBuffer(&req);

	for (attempt = 0; attempt < 3; attempt++)
	{
		size_t		sent = 0;
		bool		send_ok = true;
		char	   *rbuf = NULL;
		size_t		rlen = 0;
		size_t		rcap = 8192;

		spanner_refresh_bearer_token(conn, false);

		resetPQExpBuffer(&req);
		if (strcmp(conn->port, "443") == 0 || strcmp(conn->port, "80") == 0)
		{
			printfPQExpBuffer(&req,
							  "%s %s HTTP/1.1\r\n"
							  "Host: %s\r\n",
							  method, uri_path, conn->host);
		}
		else
		{
			printfPQExpBuffer(&req,
							  "%s %s HTTP/1.1\r\n"
							  "Host: %s:%s\r\n",
							  method, uri_path, conn->host, conn->port);
		}
		appendPQExpBuffer(&req,
						  "Connection: keep-alive\r\n"
						  "Content-Type: application/json\r\n"
						  "Accept: application/json\r\n"
						  "Content-Length: %zu\r\n",
						  body_len);
		if (conn->auth_token != NULL)
			appendPQExpBuffer(&req, "Authorization: Bearer %s\r\n", conn->auth_token);
		appendPQExpBufferStr(&req, "\r\n");
		if (body_len > 0)
			appendBinaryPQExpBuffer(&req, json_body, body_len);

		if (conn->http_sock < 0)
		{
			conn->http_sock = spanner_connect_tcp(conn);
			if (conn->http_sock < 0)
			{
				termPQExpBuffer(&req);
				return false;
			}
		}

		while (sent < req.len)
		{
			ssize_t		n = spanner_socket_write(conn, req.data + sent,
												 req.len - sent);

			if (n > 0)
			{
				sent += (size_t) n;
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			send_ok = false;
			break;
		}

		if (!send_ok)
		{
			spanner_disconnect_tcp(conn);
			continue;
		}

		rbuf = pg_malloc(rcap);
		for (;;)
		{
			const char *hdr_end = NULL;
			ssize_t		n;

			for (size_t i = 0; i + 3 < rlen; i++)
			{
				if (rbuf[i] == '\r' && rbuf[i + 1] == '\n' &&
					rbuf[i + 2] == '\r' && rbuf[i + 3] == '\n')
				{
					hdr_end = rbuf + i;
					break;
				}
			}

			if (hdr_end != NULL)
			{
				size_t		hdr_len = (size_t) (hdr_end - rbuf) + 4;
				size_t		clen = 0;
				bool		has_clen = false;
				bool		is_chunked = false;
				const char *line = rbuf;
				const char *sp = memchr(line, ' ', hdr_end - line);

				if (sp != NULL)
					*out_http_status = atoi(sp + 1);

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
						clen = (size_t) strtoull(v, NULL, 10);
						has_clen = true;
					}
					else if (pg_strncasecmp(line, "Transfer-Encoding:", 18) == 0)
					{
						const char *v = line + 18;

						while (v < eol && isspace((unsigned char) *v))
							v++;
						if (eol - v >= 7 && pg_strncasecmp(v, "chunked", 7) == 0)
							is_chunked = true;
					}
					line = (eol + 2 <= hdr_end) ? (eol + 2) : hdr_end;
				}

				if (is_chunked)
				{
					size_t		pos = hdr_len;
					PQExpBufferData cbuf;
					bool		complete = false;

					initPQExpBuffer(&cbuf);
					while (pos < rlen)
					{
						const char *line_start = rbuf + pos;
						const char *crlf = NULL;
						char	   *endp = NULL;
						size_t		chunk_sz;

						for (size_t k = pos; k + 1 < rlen; k++)
						{
							if (rbuf[k] == '\r' && rbuf[k + 1] == '\n')
							{
								crlf = rbuf + k;
								break;
							}
						}
						if (crlf == NULL)
							break;

						chunk_sz = (size_t) strtoull(line_start, &endp, 16);
						pos = (size_t) (crlf - rbuf) + 2;
						if (chunk_sz == 0)
						{
							if (pos + 2 <= rlen)
							{
								complete = true;
							}
							break;
						}
						if (pos + chunk_sz + 2 > rlen)
							break;
						appendBinaryPQExpBuffer(&cbuf, rbuf + pos, chunk_sz);
						pos += chunk_sz + 2;
					}

					if (complete)
					{
						if (*out_http_status == 401 && !retried_auth &&
							getenv("SPANNER_TOKEN_COMMAND") != NULL)
						{
							retried_auth = true;
							termPQExpBuffer(&cbuf);
							pg_free(rbuf);
							rbuf = NULL;
							spanner_refresh_bearer_token(conn, true);
							break;
						}
						*out_resp_body = cbuf.data;
						*out_resp_len = cbuf.len;
						pg_free(rbuf);
						termPQExpBuffer(&req);
						return true;
					}
					termPQExpBuffer(&cbuf);
				}
				else if (has_clen)
				{
					if (rlen >= hdr_len + clen)
					{
						if (*out_http_status == 401 && !retried_auth &&
							getenv("SPANNER_TOKEN_COMMAND") != NULL)
						{
							retried_auth = true;
							pg_free(rbuf);
							rbuf = NULL;
							spanner_refresh_bearer_token(conn, true);
							break;
						}
						*out_resp_body = pnstrdup(rbuf + hdr_len, clen);
						*out_resp_len = clen;
						pg_free(rbuf);
						termPQExpBuffer(&req);
						return true;
					}
				}
				else if (*out_http_status == 204 || *out_http_status == 304)
				{
					*out_resp_body = pg_strdup("");
					*out_resp_len = 0;
					pg_free(rbuf);
					termPQExpBuffer(&req);
					return true;
				}
			}

			if (rlen + 4096 > rcap)
			{
				rcap *= 2;
				rbuf = pg_realloc(rbuf, rcap);
			}
			n = spanner_socket_read(conn, rbuf + rlen, rcap - rlen);
			if (n > 0)
			{
				rlen += (size_t) n;
				continue;
			}
			if (n == 0 && hdr_end != NULL)
			{
				size_t		hdr_len = (size_t) (hdr_end - rbuf) + 4;
				size_t		blen = (rlen >= hdr_len) ? (rlen - hdr_len) : 0;

				*out_resp_body = pnstrdup(rbuf + hdr_len, blen);
				*out_resp_len = blen;
				pg_free(rbuf);
				spanner_disconnect_tcp(conn);
				termPQExpBuffer(&req);
				return true;
			}
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}

		if (rbuf != NULL)
		{
			pg_free(rbuf);
			spanner_disconnect_tcp(conn);
		}
	}

	printfPQExpBuffer(&conn->err_buf,
					  "Spanner HTTP RPC to %s:%s%s failed\n",
					  conn->host, conn->port, uri_path);
	termPQExpBuffer(&req);
	return false;
}

/* -------------------------------------------------------------------------
 * Spanner Session, Transaction, SQL, and Result Execution Logic
 * -------------------------------------------------------------------------
 */

static PgBenchResult *
spanner_parse_error_response(int http_status, const char *resp, size_t len)
{
	const char *end = resp + len;
	const char *err_obj = sp_json_find_field(resp, end, "error");
	char	   *msg = NULL;
	char	   *status_str = NULL;
	const char *sqlstate = "XX000";
	PgBenchResult *res;

	if (err_obj != NULL && *err_obj == '{')
	{
		msg = sp_json_extract_string_field(err_obj, end, "message");
		status_str = sp_json_extract_string_field(err_obj, end, "status");
	}
	if (msg == NULL)
		msg = (len > 0) ? pnstrdup(resp, len) : pg_strdup("Spanner RPC failed");

	if (http_status == 409 ||
		(status_str != NULL && strcmp(status_str, "ABORTED") == 0))
		sqlstate = "40001";
	else if (http_status == 404 ||
			 (status_str != NULL && strcmp(status_str, "NOT_FOUND") == 0))
		sqlstate = "42P01";
	else if (http_status == 400 ||
			 (status_str != NULL && strcmp(status_str, "INVALID_ARGUMENT") == 0))
		sqlstate = "42601";

	res = spanner_make_result(PGBENCH_RES_FATAL_ERROR, msg, sqlstate);
	pg_free(msg);
	pg_free(status_str);
	return res;
}

static bool
spanner_ensure_session(PgBenchConn *conn)
{
	PQExpBufferData path;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	char	   *sname;

	if (conn->session_name != NULL)
		return true;

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s/sessions", conn->db_resource);

	if (!spanner_http_rpc(conn, "POST", path.data, "{}",
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		return false;
	}
	termPQExpBuffer(&path);

	if (http_status >= 400 || resp == NULL)
	{
		PgBenchResult *er = spanner_parse_error_response(http_status,
														 resp ? resp : "", rlen);

		printfPQExpBuffer(&conn->err_buf, "%s\n",
						  er->err_msg ? er->err_msg : "CreateSession failed");
		spanner_free_result(er);
		pg_free(resp);
		return false;
	}

	sname = sp_json_extract_string_field(resp, resp + rlen, "name");
	pg_free(resp);
	if (sname == NULL)
		sname = psprintf("%s/sessions/default-session", conn->db_resource);

	conn->session_name = sname;
	return true;
}

static void
spanner_clear_precommit_token(PgBenchConn *conn)
{
	pg_free(conn->precommit_token);
	conn->precommit_token = NULL;
	conn->precommit_seq = 0;
}

static void
spanner_update_precommit_token(PgBenchConn *conn, const char *json,
							   const char *end)
{
	const char *pt_obj;
	const char *pt_end;
	char	   *tok;
	const char *seq_v;
	int64		seq = 0;

	if (conn == NULL || json == NULL || json >= end)
		return;

	pt_obj = sp_json_find_field(json, end, "precommitToken");
	if (pt_obj == NULL || *pt_obj != '{')
		return;

	pt_end = sp_json_skip_value(pt_obj, end);
	tok = sp_json_extract_string_field(pt_obj, pt_end, "precommitToken");
	if (tok == NULL)
		return;

	seq_v = sp_json_find_field(pt_obj, pt_end, "seqNum");
	if (seq_v != NULL)
	{
		if (*seq_v == '"')
			seq = strtoll(seq_v + 1, NULL, 10);
		else
			seq = strtoll(seq_v, NULL, 10);
	}

	if (conn->precommit_token == NULL || seq >= conn->precommit_seq)
	{
		pg_free(conn->precommit_token);
		conn->precommit_token = tok;
		conn->precommit_seq = seq;
	}
	else
		pg_free(tok);
}

static void
spanner_rpc_delete_session(PgBenchConn *conn)
{
	PQExpBufferData path;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;

	if (conn == NULL || conn->session_name == NULL || *conn->session_name == '\0')
		return;

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s", conn->session_name);
	(void) spanner_http_rpc(conn, "DELETE", path.data, NULL,
							&http_status, &resp, &rlen);
	pg_free(resp);
	termPQExpBuffer(&path);
	pg_free(conn->session_name);
	conn->session_name = NULL;
}

static const char *
spanner_skip_sql_ws_and_comments(const char *p)
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

static bool
spanner_rpc_begin_rw_tx(PgBenchConn *conn, char **out_tx_id,
						PgBenchResult **out_err)
{
	PQExpBufferData path;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	char	   *tx_id;

	*out_tx_id = NULL;
	if (out_err)
		*out_err = NULL;

	spanner_clear_precommit_token(conn);

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:beginTransaction", conn->session_name);
	if (!spanner_http_rpc(conn, "POST", path.data,
						  "{\"options\": {\"readWrite\": {}}}",
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		if (out_err)
			*out_err = spanner_make_result(PGBENCH_RES_FATAL_ERROR,
										   conn->err_buf.data, "08006");
		return false;
	}
	termPQExpBuffer(&path);

	if (http_status >= 400)
	{
		if (out_err)
			*out_err = spanner_parse_error_response(http_status,
													resp ? resp : "", rlen);
		pg_free(resp);
		return false;
	}

	spanner_update_precommit_token(conn, resp, resp + rlen);
	tx_id = sp_json_extract_string_field(resp, resp + rlen, "id");
	pg_free(resp);
	if (tx_id == NULL)
		tx_id = pg_strdup("tx-default");
	*out_tx_id = tx_id;
	return true;
}

static bool
spanner_rpc_commit_tx(PgBenchConn *conn, const char *tx_id,
					  PgBenchResult **out_err)
{
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;

	if (out_err)
		*out_err = NULL;

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:commit", conn->session_name);

	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body, "{\"transactionId\": ");
	spanner_append_json_escaped(&body, tx_id);
	if (conn->precommit_token != NULL)
	{
		appendPQExpBufferStr(&body, ", \"precommitToken\": {\"precommitToken\": ");
		spanner_append_json_escaped(&body, conn->precommit_token);
		appendPQExpBuffer(&body, ", \"seqNum\": " INT64_FORMAT "}", conn->precommit_seq);
	}
	appendPQExpBufferChar(&body, '}');
	spanner_clear_precommit_token(conn);

	if (!spanner_http_rpc(conn, "POST", path.data, body.data,
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		termPQExpBuffer(&body);
		if (out_err)
			*out_err = spanner_make_result(PGBENCH_RES_FATAL_ERROR,
										   conn->err_buf.data, "08006");
		return false;
	}
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);

	if (http_status >= 400)
	{
		if (out_err)
			*out_err = spanner_parse_error_response(http_status,
													resp ? resp : "", rlen);
		pg_free(resp);
		return false;
	}
	pg_free(resp);
	return true;
}

static void
spanner_rpc_rollback_tx(PgBenchConn *conn, const char *tx_id)
{
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;

	spanner_clear_precommit_token(conn);

	if (tx_id == NULL)
		return;

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:rollback", conn->session_name);

	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body, "{\"transactionId\": ");
	spanner_append_json_escaped(&body, tx_id);
	appendPQExpBufferChar(&body, '}');

	(void) spanner_http_rpc(conn, "POST", path.data, body.data,
							&http_status, &resp, &rlen);
	pg_free(resp);
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);
}

/*
 * Parse Spanner ResultSet JSON into PgBenchResult:
 *   {
 *     "metadata": {
 *       "rowType": { "fields": [ {"name": "col1", "type": {"code": "INT64"}}, ... ] },
 *       "transaction": { "id": "..." }
 *     },
 *     "rows": [ ["val1", "val2"], ... ],
 *     "stats": { "rowCountExact": "1" }
 *   }
 */
static PgBenchResult *
spanner_parse_result_set(PgBenchConn *conn, const char *resp, size_t len,
						 char **out_inline_tx_id)
{
	const char *end = resp + len;
	const char *meta = sp_json_find_field(resp, end, "metadata");
	const char *rows_arr = sp_json_find_field(resp, end, "rows");
	const char *stats_obj = sp_json_find_field(resp, end, "stats");
	const char *fields_arr = NULL;
	int			affected_rows = 0;
	PgBenchResult *res;

	if (out_inline_tx_id)
		*out_inline_tx_id = NULL;

	spanner_update_precommit_token(conn, resp, end);

	if (stats_obj != NULL && *stats_obj == '{')
	{
		const char *st_end = sp_json_skip_value(stats_obj, end);
		const char *rc_v = sp_json_find_field(stats_obj, st_end, "rowCountExact");

		if (rc_v == NULL)
			rc_v = sp_json_find_field(stats_obj, st_end, "rowCountLowerBound");
		if (rc_v != NULL)
		{
			if (*rc_v == '"')
				affected_rows = atoi(rc_v + 1);
			else
				affected_rows = atoi(rc_v);
		}
	}

	if (meta != NULL && *meta == '{')
	{
		const char *meta_end = sp_json_skip_value(meta, end);
		const char *row_type = sp_json_find_field(meta, meta_end, "rowType");
		const char *tx_obj = sp_json_find_field(meta, meta_end, "transaction");

		if (row_type != NULL && *row_type == '{')
		{
			const char *rt_end = sp_json_skip_value(row_type, meta_end);

			fields_arr = sp_json_find_field(row_type, rt_end, "fields");
		}
		if (tx_obj != NULL && *tx_obj == '{')
		{
			const char *tx_end = sp_json_skip_value(tx_obj, meta_end);

			spanner_update_precommit_token(conn, tx_obj, tx_end);
			if (out_inline_tx_id != NULL && *out_inline_tx_id == NULL)
				*out_inline_tx_id = sp_json_extract_string_field(tx_obj, tx_end, "id");
		}
	}

	if (fields_arr == NULL || *fields_arr != '[')
	{
		res = spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
		res->nrows = affected_rows;
		return res;
	}

	res = spanner_make_result(PGBENCH_RES_TUPLES_OK, NULL, NULL);
	{
		const char *p = sp_json_skip_ws(fields_arr + 1, end);
		int			col_cap = 8;

		res->col_names = pg_malloc0_array(char *, col_cap);
		while (p < end && *p != ']')
		{
			if (*p == '{')
			{
				const char *f_end = sp_json_skip_value(p, end);
				char	   *fname = sp_json_extract_string_field(p, f_end, "name");

				if (res->ncols >= col_cap)
				{
					col_cap *= 2;
					res->col_names = pg_realloc_array(res->col_names, char *, col_cap);
				}
				{
					int			col_idx = res->ncols++;

					res->col_names[col_idx] = fname ? fname : psprintf("col_%d", col_idx);
				}
				p = f_end;
			}
			else
				p++;
			p = sp_json_skip_ws(p, end);
			if (p < end && *p == ',')
				p = sp_json_skip_ws(p + 1, end);
		}
	}

	if (res->ncols == 0)
	{
		res->status = PGBENCH_RES_COMMAND_OK;
		res->nrows = affected_rows;
		return res;
	}

	if (rows_arr != NULL && *rows_arr == '[')
	{
		const char *p = sp_json_skip_ws(rows_arr + 1, end);
		int			row_cap = 16;

		res->cells = pg_malloc0_array(char **, row_cap);
		while (p < end && *p != ']')
		{
			if (*p == '[')
			{
				char	  **row_cells = pg_malloc0_array(char *, res->ncols);
				int			cidx = 0;

				p = sp_json_skip_ws(p + 1, end);
				while (p < end && *p != ']')
				{
					if (*p == '"')
					{
						char	   *s = sp_json_parse_string(&p, end);

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

						p = sp_json_skip_value(p, end);
						if (cidx < res->ncols)
							row_cells[cidx++] = pnstrdup(vstart, p - vstart);
					}
					p = sp_json_skip_ws(p, end);
					if (p < end && *p == ',')
						p = sp_json_skip_ws(p + 1, end);
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
			p = sp_json_skip_ws(p, end);
			if (p < end && *p == ',')
				p = sp_json_skip_ws(p + 1, end);
		}
	}

	return res;
}

static PgBenchResult *
spanner_execute_ddl_batch(PgBenchConn *conn, char **stmts, int nstmts)
{
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	int			poll_count = 0;
	PgBenchResult *res;

	if (nstmts <= 0)
		return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s/ddl", conn->db_resource);
	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body, "{\"statements\": [");
	for (int i = 0; i < nstmts; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(&body, ", ");
		spanner_append_json_escaped(&body, stmts[i]);
	}
	appendPQExpBufferStr(&body, "]}");

	if (!spanner_http_rpc(conn, "PATCH", path.data, body.data,
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		termPQExpBuffer(&body);
		return spanner_make_result(PGBENCH_RES_FATAL_ERROR,
								   conn->err_buf.data, "08006");
	}
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);

	if (http_status >= 400)
	{
		res = spanner_parse_error_response(http_status,
										   resp ? resp : "", rlen);
		pg_free(resp);
		return res;
	}

	/* Poll Long-Running Operation (LRO) if done == false, and check error */
	while (resp != NULL)
	{
		const char *end = resp + rlen;
		const char *err_obj = sp_json_find_field(resp, end, "error");
		const char *done_v = sp_json_find_field(resp, end, "done");
		bool		is_done = true;

		if (err_obj != NULL && *err_obj == '{')
		{
			res = spanner_parse_error_response(400, resp, rlen);
			pg_free(resp);
			return res;
		}
		if (done_v != NULL && end - done_v >= 5 &&
			strncmp(done_v, "false", 5) == 0)
			is_done = false;

		if (is_done)
			break;

		{
			char	   *op_name = sp_json_extract_string_field(resp, end, "name");
			PQExpBufferData op_path;

			pg_free(resp);
			resp = NULL;
			rlen = 0;

			if (op_name == NULL || *op_name == '\0')
			{
				pg_free(op_name);
				break;
			}

			if (poll_count > 0)
				usleep(150000);
			poll_count++;

			initPQExpBuffer(&op_path);
			if (strncmp(op_name, "/v1/", 4) == 0)
				appendPQExpBufferStr(&op_path, op_name);
			else
				printfPQExpBuffer(&op_path, "/v1/%s", op_name);
			pg_free(op_name);

			if (!spanner_http_rpc(conn, "GET", op_path.data, NULL,
								  &http_status, &resp, &rlen))
			{
				termPQExpBuffer(&op_path);
				return spanner_make_result(PGBENCH_RES_FATAL_ERROR,
										   conn->err_buf.data, "08006");
			}
			termPQExpBuffer(&op_path);

			if (http_status >= 400)
			{
				res = spanner_parse_error_response(http_status,
												   resp ? resp : "", rlen);
				pg_free(resp);
				return res;
			}
		}
	}

	pg_free(resp);
	return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
}

static PgBenchResult *
spanner_execute_ddl_script(PgBenchConn *conn, const char *sql)
{
	int			cap = 16;
	int			nstmts = 0;
	char	  **stmts = pg_malloc0_array(char *, cap);
	int			batch_is_drop = -1;
	const char *cur = sql;
	const char *seg_start = sql;
	PgBenchResult *res = NULL;

	while (*cur != '\0')
	{
		if (*cur == '\'' || *cur == '"' || *cur == '`')
		{
			char		q = *cur++;

			while (*cur != '\0')
			{
				if (*cur == '\\' && cur[1] != '\0')
					cur += 2;
				else if (*cur == q)
				{
					cur++;
					if (*cur == q)
						cur++;
					else
						break;
				}
				else
					cur++;
			}
		}
		else if (cur[0] == '-' && cur[1] == '-')
		{
			cur += 2;
			while (*cur != '\0' && *cur != '\n')
				cur++;
		}
		else if (cur[0] == '/' && cur[1] == '*')
		{
			cur += 2;
			while (*cur != '\0' && !(cur[0] == '*' && cur[1] == '/'))
				cur++;
			if (*cur != '\0')
				cur += 2;
		}
		else if (*cur == ';' || cur[1] == '\0')
		{
			size_t		seg_len = (*cur == ';') ? (size_t) (cur - seg_start)
												: (size_t) (cur + 1 - seg_start);
			char	   *raw = pnstrdup(seg_start, seg_len);
			const char *sp = spanner_skip_sql_ws_and_comments(raw);

			if (sp != NULL && *sp != '\0')
			{
				char	   *trimmed = pg_strdup(sp);
				size_t		tlen = strlen(trimmed);

				while (tlen > 0 &&
					   (isspace((unsigned char) trimmed[tlen - 1]) ||
						trimmed[tlen - 1] == ';'))
					trimmed[--tlen] = '\0';

				if (tlen > 0 &&
					!((pg_strncasecmp(trimmed, "VACUUM", 6) == 0 &&
					   (trimmed[6] == '\0' || isspace((unsigned char) trimmed[6]))) ||
					  (pg_strncasecmp(trimmed, "ANALYZE", 7) == 0 &&
					   (trimmed[7] == '\0' || isspace((unsigned char) trimmed[7]))) ||
					  (pg_strncasecmp(trimmed, "SET", 3) == 0 &&
					   isspace((unsigned char) trimmed[3]))))
				{
					int			is_drop = (pg_strncasecmp(trimmed, "DROP", 4) == 0 &&
										   isspace((unsigned char) trimmed[4])) ? 1 : 0;

					if (batch_is_drop != -1 && batch_is_drop != is_drop && nstmts > 0)
					{
						res = spanner_execute_ddl_batch(conn, stmts, nstmts);
						for (int i = 0; i < nstmts; i++)
							pg_free(stmts[i]);
						nstmts = 0;
						if (res->status != PGBENCH_RES_COMMAND_OK)
						{
							pg_free(trimmed);
							pg_free(raw);
							pg_free(stmts);
							return res;
						}
						spanner_free_result(res);
						res = NULL;
					}
					batch_is_drop = is_drop;
					if (nstmts >= cap)
					{
						cap *= 2;
						stmts = pg_realloc_array(stmts, char *, cap);
					}
					stmts[nstmts++] = trimmed;
				}
				else
					pg_free(trimmed);
			}
			pg_free(raw);
			cur++;
			seg_start = cur;
		}
		else
			cur++;
	}

	if (nstmts > 0)
	{
		res = spanner_execute_ddl_batch(conn, stmts, nstmts);
		for (int i = 0; i < nstmts; i++)
			pg_free(stmts[i]);
	}
	else
		res = spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);

	pg_free(stmts);
	return res;
}

static PgBenchResult *
spanner_execute_sql_task(PgBenchConn *conn, const char *sql,
						 const char *params_json, const char *param_types_json)
{
	const char *p;
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	char	   *rewritten_sql = NULL;
	bool		is_select = false;
	bool		is_ddl = false;
	bool		is_autocommit_dml = false;
	char	   *inline_tx_id = NULL;
	PgBenchResult *res;

	if (!spanner_ensure_session(conn))
		return spanner_make_result(PGBENCH_RES_FATAL_ERROR,
								   conn->err_buf.data, "08006");

	p = spanner_skip_sql_ws_and_comments(sql);
	if (p == NULL || *p == '\0')
		return spanner_make_result(PGBENCH_RES_EMPTY_QUERY, NULL, NULL);

	/* Transaction control statements */
	if ((pg_strncasecmp(p, "BEGIN", 5) == 0 &&
		 (p[5] == '\0' || p[5] == ';' || isspace((unsigned char) p[5]) ||
		  p[5] == '-' || p[5] == '/')) ||
		pg_strncasecmp(p, "START TRANSACTION", 17) == 0)
	{
		PgBenchResult *err_res = NULL;
		char	   *tx_id = NULL;

		if (!spanner_rpc_begin_rw_tx(conn, &tx_id, &err_res))
			return err_res;
		pg_free(conn->active_tx_id);
		conn->active_tx_id = tx_id;
		conn->tx_seqno = 0;
		pthread_mutex_lock(&conn->mu);
		conn->tx_status = PGBENCH_TX_INTRANS;
		pthread_mutex_unlock(&conn->mu);
		return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
	}

	if ((pg_strncasecmp(p, "COMMIT", 6) == 0 &&
		 (p[6] == '\0' || p[6] == ';' || isspace((unsigned char) p[6]) ||
		  p[6] == '-' || p[6] == '/')) ||
		(pg_strncasecmp(p, "END", 3) == 0 &&
		 (p[3] == '\0' || p[3] == ';' || isspace((unsigned char) p[3]) ||
		  p[3] == '-' || p[3] == '/')))
	{
		PgBenchResult *err_res = NULL;
		char	   *tx_id = conn->active_tx_id;

		conn->active_tx_id = NULL;
		conn->tx_seqno = 0;
		pthread_mutex_lock(&conn->mu);
		conn->tx_status = PGBENCH_TX_IDLE;
		pthread_mutex_unlock(&conn->mu);
		if (tx_id == NULL)
			return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
		if (!spanner_rpc_commit_tx(conn, tx_id, &err_res))
		{
			pg_free(tx_id);
			return err_res;
		}
		pg_free(tx_id);
		return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
	}

	if (pg_strncasecmp(p, "ROLLBACK", 8) == 0 &&
		(p[8] == '\0' || p[8] == ';' || isspace((unsigned char) p[8]) ||
		 p[8] == '-' || p[8] == '/'))
	{
		char	   *tx_id = conn->active_tx_id;

		conn->active_tx_id = NULL;
		conn->tx_seqno = 0;
		pthread_mutex_lock(&conn->mu);
		conn->tx_status = PGBENCH_TX_IDLE;
		pthread_mutex_unlock(&conn->mu);
		if (tx_id != NULL)
		{
			spanner_rpc_rollback_tx(conn, tx_id);
			pg_free(tx_id);
		}
		return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
	}

	/* VACUUM, ANALYZE, and SET are no-ops on Spanner */
	if ((pg_strncasecmp(p, "VACUUM", 6) == 0 &&
		 (p[6] == '\0' || p[6] == ';' || isspace((unsigned char) p[6]))) ||
		(pg_strncasecmp(p, "ANALYZE", 7) == 0 &&
		 (p[7] == '\0' || p[7] == ';' || isspace((unsigned char) p[7]))) ||
		(pg_strncasecmp(p, "SET", 3) == 0 && isspace((unsigned char) p[3])))
		return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);

	/* Translate TRUNCATE [TABLE] <tbl> into DELETE FROM <tbl> WHERE true */
	if (pg_strncasecmp(p, "TRUNCATE", 8) == 0 && isspace((unsigned char) p[8]))
	{
		const char *tp = p + 8;

		while (*tp && isspace((unsigned char) *tp))
			tp++;
		if (pg_strncasecmp(tp, "TABLE", 5) == 0 && isspace((unsigned char) tp[5]))
		{
			tp += 5;
			while (*tp && isspace((unsigned char) *tp))
				tp++;
		}
		rewritten_sql = psprintf("DELETE FROM %s WHERE true", tp);
		sql = rewritten_sql;
		p = sql;
	}

	if (pg_strncasecmp(p, "SELECT", 6) == 0 ||
		pg_strncasecmp(p, "WITH", 4) == 0 ||
		pg_strncasecmp(p, "GRAPH", 5) == 0)
		is_select = true;
	else if (pg_strncasecmp(p, "CREATE", 6) == 0 ||
			 pg_strncasecmp(p, "DROP", 4) == 0 ||
			 pg_strncasecmp(p, "ALTER", 5) == 0)
		is_ddl = true;

	/* Dispatch DDL via UpdateDatabaseDdl (/v1/{db}/ddl) + poll LRO until done */
	if (is_ddl)
	{
		res = spanner_execute_ddl_script(conn, p);
		pg_free(rewritten_sql);
		return res;
	}

	/* Build ExecuteSql request */
	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:executeSql", conn->session_name);

	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body, "{\"sql\": ");
	spanner_append_json_escaped(&body, sql);

	if (conn->active_tx_id != NULL)
	{
		conn->tx_seqno++;
		appendPQExpBufferStr(&body, ", \"transaction\": {\"id\": ");
		spanner_append_json_escaped(&body, conn->active_tx_id);
		appendPQExpBuffer(&body, "}, \"seqno\": \"" INT64_FORMAT "\"", conn->tx_seqno);
	}
	else if (is_select)
	{
		appendPQExpBufferStr(&body,
							 ", \"transaction\": {\"singleUse\": {\"readOnly\": {\"strong\": true}}}");
	}
	else
	{
		is_autocommit_dml = true;
		spanner_clear_precommit_token(conn);
		appendPQExpBufferStr(&body,
							 ", \"transaction\": {\"begin\": {\"readWrite\": {}}}"
							 ", \"seqno\": \"1\"");
	}

	if (params_json != NULL && param_types_json != NULL)
	{
		appendPQExpBuffer(&body, ", \"params\": %s, \"paramTypes\": %s",
						  params_json, param_types_json);
	}
	appendPQExpBufferChar(&body, '}');

	if (!spanner_http_rpc(conn, "POST", path.data, body.data,
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		termPQExpBuffer(&body);
		pg_free(rewritten_sql);
		return spanner_make_result(PGBENCH_RES_FATAL_ERROR,
								   conn->err_buf.data, "08006");
	}
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);
	pg_free(rewritten_sql);

	if (http_status >= 400)
	{
		res = spanner_parse_error_response(http_status,
										   resp ? resp : "", rlen);
		pg_free(resp);
		if (conn->active_tx_id != NULL)
		{
			pthread_mutex_lock(&conn->mu);
			conn->tx_status = PGBENCH_TX_INERROR;
			pthread_mutex_unlock(&conn->mu);
		}
		return res;
	}

	res = spanner_parse_result_set(conn, resp, rlen, &inline_tx_id);
	pg_free(resp);

	if (is_autocommit_dml && inline_tx_id != NULL)
	{
		PgBenchResult *commit_err = NULL;

		if (!spanner_rpc_commit_tx(conn, inline_tx_id, &commit_err))
		{
			pg_free(inline_tx_id);
			spanner_free_result(res);
			return commit_err;
		}
	}
	pg_free(inline_tx_id);

	return res;
}

static void
spanner_execute_batch_dml_task(PgBenchConn *conn, SpannerBatchStmt *stmts,
							   int nstmts, PgBenchResult ***out_results,
							   int *out_nresults)
{
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	bool		is_autocommit = (conn->active_tx_id == NULL);
	char	   *inline_tx_id = NULL;
	PgBenchResult **results = pg_malloc0_array(PgBenchResult *, nstmts);

	*out_results = results;
	*out_nresults = nstmts;

	if (!spanner_ensure_session(conn))
	{
		for (int i = 0; i < nstmts; i++)
			results[i] = (i == 0) ?
				spanner_make_result(PGBENCH_RES_FATAL_ERROR, conn->err_buf.data, "08006") :
				spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
		return;
	}

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:executeBatchDml", conn->session_name);

	initPQExpBuffer(&body);
	appendPQExpBufferChar(&body, '{');
	if (conn->active_tx_id != NULL)
	{
		conn->tx_seqno++;
		appendPQExpBufferStr(&body, "\"transaction\": {\"id\": ");
		spanner_append_json_escaped(&body, conn->active_tx_id);
		appendPQExpBuffer(&body, "}, \"seqno\": \"" INT64_FORMAT "\", ", conn->tx_seqno);
	}
	else
	{
		spanner_clear_precommit_token(conn);
		appendPQExpBufferStr(&body,
							 "\"transaction\": {\"begin\": {\"readWrite\": {}}}, "
							 "\"seqno\": \"1\", ");
	}

	appendPQExpBufferStr(&body, "\"statements\": [");
	for (int i = 0; i < nstmts; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(&body, ", ");
		appendPQExpBufferStr(&body, "{\"sql\": ");
		spanner_append_json_escaped(&body, stmts[i].sql);
		if (stmts[i].params_json != NULL && stmts[i].param_types_json != NULL)
		{
			appendPQExpBuffer(&body, ", \"params\": %s, \"paramTypes\": %s",
							  stmts[i].params_json, stmts[i].param_types_json);
		}
		appendPQExpBufferChar(&body, '}');
	}
	appendPQExpBufferStr(&body, "]}");

	if (!spanner_http_rpc(conn, "POST", path.data, body.data,
						  &http_status, &resp, &rlen))
	{
		termPQExpBuffer(&path);
		termPQExpBuffer(&body);
		for (int i = 0; i < nstmts; i++)
			results[i] = (i == 0) ?
				spanner_make_result(PGBENCH_RES_FATAL_ERROR, conn->err_buf.data, "08006") :
				spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
		return;
	}
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);

	if (http_status >= 400)
	{
		PgBenchResult *er = spanner_parse_error_response(http_status,
														 resp ? resp : "", rlen);

		pg_free(resp);
		if (conn->active_tx_id != NULL)
		{
			pthread_mutex_lock(&conn->mu);
			conn->tx_status = PGBENCH_TX_INERROR;
			pthread_mutex_unlock(&conn->mu);
		}
		results[0] = er;
		for (int i = 1; i < nstmts; i++)
			results[i] = spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
		return;
	}

	/* Check embedded status and resultSets in ExecuteBatchDmlResponse */
	{
		const char *end = resp + rlen;
		const char *st_obj = sp_json_find_field(resp, end, "status");
		const char *rs_arr = sp_json_find_field(resp, end, "resultSets");

		spanner_update_precommit_token(conn, resp, end);

		if (rs_arr != NULL && *rs_arr == '[')
		{
			const char *p = sp_json_skip_ws(rs_arr + 1, end);
			int			rs_idx = 0;

			while (p < end && *p != ']' && rs_idx < nstmts)
			{
				if (*p == '{')
				{
					const char *rs_end = sp_json_skip_value(p, end);
					char	   *rs_tx_id = NULL;
					PgBenchResult *rs_res = spanner_parse_result_set(conn, p,
																	 rs_end - p,
																	 &rs_tx_id);

					if (rs_tx_id != NULL)
					{
						if (inline_tx_id == NULL)
							inline_tx_id = rs_tx_id;
						else
							pg_free(rs_tx_id);
					}
					results[rs_idx++] = rs_res;
					p = rs_end;
				}
				else
					p++;
				p = sp_json_skip_ws(p, end);
				if (p < end && *p == ',')
					p = sp_json_skip_ws(p + 1, end);
			}
		}

		if (st_obj != NULL && *st_obj == '{')
		{
			const char *st_end = sp_json_skip_value(st_obj, end);
			const char *code_v = sp_json_find_field(st_obj, st_end, "code");
			int			grpc_code = code_v ? atoi(code_v) : 0;

			if (grpc_code != 0)
			{
				char	   *emsg = sp_json_extract_string_field(st_obj, st_end, "message");
				const char *sqlstate = (grpc_code == 10) ? "40001" : "XX000";

				for (int i = 0; i < nstmts; i++)
				{
					spanner_free_result(results[i]);
					results[i] = NULL;
				}
				if (conn->active_tx_id != NULL)
				{
					pthread_mutex_lock(&conn->mu);
					conn->tx_status = PGBENCH_TX_INERROR;
					pthread_mutex_unlock(&conn->mu);
				}
				results[0] = spanner_make_result(PGBENCH_RES_FATAL_ERROR,
												 emsg ? emsg : "BatchDml failed",
												 sqlstate);
				pg_free(emsg);
				for (int i = 1; i < nstmts; i++)
					results[i] = spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
				pg_free(inline_tx_id);
				pg_free(resp);
				return;
			}
		}
	}
	pg_free(resp);

	if (is_autocommit && inline_tx_id != NULL)
	{
		PgBenchResult *commit_err = NULL;

		if (!spanner_rpc_commit_tx(conn, inline_tx_id, &commit_err))
		{
			pg_free(inline_tx_id);
			for (int i = 0; i < nstmts; i++)
			{
				spanner_free_result(results[i]);
				results[i] = NULL;
			}
			results[0] = commit_err;
			for (int i = 1; i < nstmts; i++)
				results[i] = spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
			return;
		}
	}
	pg_free(inline_tx_id);

	for (int i = 0; i < nstmts; i++)
	{
		if (results[i] == NULL)
			results[i] = spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
	}
}

/* -------------------------------------------------------------------------
 * Async Worker Thread & eventfd Signaling
 * -------------------------------------------------------------------------
 */

static void
spanner_signal_wakeup(PgBenchConn *conn)
{
	if (conn->wakeup_write_fd >= 0)
	{
		char		b = 1;

		if (write(conn->wakeup_write_fd, &b, 1) < 0)
		{
			/* best-effort non-blocking wakeup */
		}
	}
	else if (conn->wakeup_fd >= 0)
	{
		uint64		val = 1;

		if (write(conn->wakeup_fd, &val, sizeof(val)) < 0)
		{
			/* best-effort non-blocking wakeup */
		}
	}
}

static void
spanner_drain_wakeup(PgBenchConn *conn)
{
	char		buf[64];

	if (conn->wakeup_fd < 0)
		return;
	while (read(conn->wakeup_fd, buf, sizeof(buf)) > 0)
	{
		/* drain all pending wakeup notifications */
	}
}

static void
spanner_push_completed_result_locked(PgBenchConn *conn, PgBenchResult *res)
{
	if (conn->pipe_aborted && res->status != PGBENCH_RES_PIPELINE_SYNC)
	{
		spanner_free_result(res);
		res = spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
	}
	else if (res->status == PGBENCH_RES_FATAL_ERROR)
	{
		resetPQExpBuffer(&conn->err_buf);
		if (res->err_msg != NULL)
			appendPQExpBuffer(&conn->err_buf, "%s\n", res->err_msg);
		if (conn->pipe_status == PGBENCH_PIPE_ON)
		{
			conn->pipe_status = PGBENCH_PIPE_ABORTED;
			conn->pipe_aborted = true;
		}
	}
	else if (res->status == PGBENCH_RES_PIPELINE_SYNC)
	{
		conn->pipe_aborted = false;
		if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
			conn->pipe_status = PGBENCH_PIPE_ON;
	}

	res->next = NULL;
	if (conn->res_tail != NULL)
		conn->res_tail->next = res;
	else
		conn->res_head = res;
	conn->res_tail = res;
}

static void *
spanner_worker_main(void *arg)
{
	PgBenchConn *conn = (PgBenchConn *) arg;

	for (;;)
	{
		SpannerTask *task;

		pthread_mutex_lock(&conn->mu);
		while (conn->task_head == NULL && !conn->worker_stop)
			pthread_cond_wait(&conn->cv, &conn->mu);

		if (conn->worker_stop && conn->task_head == NULL)
		{
			pthread_mutex_unlock(&conn->mu);
			break;
		}

		task = conn->task_head;
		conn->task_head = task->next;
		if (conn->task_head == NULL)
			conn->task_tail = NULL;
		pthread_mutex_unlock(&conn->mu);

		if (task->kind == SPANNER_TASK_CONNECT)
		{
			bool		ok = spanner_ensure_session(conn);

			pthread_mutex_lock(&conn->mu);
			conn->conn_status = ok ? PGBENCH_CONN_OK : PGBENCH_CONN_POLLING_FAILED;
			conn->tasks_in_flight--;
			pthread_cond_broadcast(&conn->cv);
			pthread_mutex_unlock(&conn->mu);
			spanner_signal_wakeup(conn);
		}
		else if (task->kind == SPANNER_TASK_PREPARE)
		{
			PgBenchResult *res = spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);

			pthread_mutex_lock(&conn->mu);
			spanner_push_completed_result_locked(conn, res);
			conn->tasks_in_flight--;
			pthread_cond_broadcast(&conn->cv);
			pthread_mutex_unlock(&conn->mu);
			spanner_signal_wakeup(conn);
		}
		else if (task->kind == SPANNER_TASK_PIPELINE_SYNC)
		{
			PgBenchResult *res = spanner_make_result(PGBENCH_RES_PIPELINE_SYNC, NULL, NULL);

			pthread_mutex_lock(&conn->mu);
			spanner_push_completed_result_locked(conn, res);
			conn->tasks_in_flight--;
			pthread_cond_broadcast(&conn->cv);
			pthread_mutex_unlock(&conn->mu);
			spanner_signal_wakeup(conn);
		}
		else if (task->kind == SPANNER_TASK_BATCH_DML)
		{
			PgBenchResult **results = NULL;
			int			nresults = 0;

			spanner_execute_batch_dml_task(conn, task->batch_stmts,
										   task->num_batch_stmts,
										   &results, &nresults);
			pthread_mutex_lock(&conn->mu);
			for (int i = 0; i < nresults; i++)
				spanner_push_completed_result_locked(conn, results[i]);
			pg_free(results);
			conn->tasks_in_flight--;
			pthread_cond_broadcast(&conn->cv);
			pthread_mutex_unlock(&conn->mu);
			spanner_signal_wakeup(conn);
		}
		else if (task->kind == SPANNER_TASK_QUERY)
		{
			PgBenchResult *res;

			pthread_mutex_lock(&conn->mu);
			if (conn->pipe_aborted)
			{
				res = spanner_make_result(PGBENCH_RES_PIPELINE_ABORTED, NULL, NULL);
				spanner_push_completed_result_locked(conn, res);
				conn->tasks_in_flight--;
				pthread_cond_broadcast(&conn->cv);
				pthread_mutex_unlock(&conn->mu);
				spanner_signal_wakeup(conn);
			}
			else
			{
				pthread_mutex_unlock(&conn->mu);
				res = spanner_execute_sql_task(conn, task->sql,
											   task->params_json,
											   task->param_types_json);
				pthread_mutex_lock(&conn->mu);
				spanner_push_completed_result_locked(conn, res);
				conn->tasks_in_flight--;
				pthread_cond_broadcast(&conn->cv);
				pthread_mutex_unlock(&conn->mu);
				spanner_signal_wakeup(conn);
			}
		}

		pg_free(task->sql);
		pg_free(task->params_json);
		pg_free(task->param_types_json);
		if (task->batch_stmts != NULL)
		{
			for (int i = 0; i < task->num_batch_stmts; i++)
			{
				pg_free(task->batch_stmts[i].sql);
				pg_free(task->batch_stmts[i].params_json);
				pg_free(task->batch_stmts[i].param_types_json);
			}
			pg_free(task->batch_stmts);
		}
		pg_free(task);
	}

	return NULL;
}

static void
spanner_enqueue_task(PgBenchConn *conn, SpannerTask *task)
{
	pthread_mutex_lock(&conn->mu);
	if (conn->pipe_status == PGBENCH_PIPE_OFF)
		conn->awaiting_result_null = false;
	task->next = NULL;
	if (conn->task_tail != NULL)
		conn->task_tail->next = task;
	else
		conn->task_head = task;
	conn->task_tail = task;
	conn->tasks_in_flight++;
	pthread_cond_signal(&conn->cv);
	pthread_mutex_unlock(&conn->mu);
}

/* -------------------------------------------------------------------------
 * PgBenchDbDriver Vtable Methods for Spanner
 * -------------------------------------------------------------------------
 */

static bool
spanner_init_global(const char *conninfo, int nthreads_arg, int nclients_arg)
{
	(void) conninfo;
	(void) nthreads_arg;
	(void) nclients_arg;
	return true;
}

static void
spanner_shutdown_global(void)
{
}

static PgBenchConn *
spanner_alloc_conn(const char *host, const char *port,
				   const char *user, const char *dbname)
{
	PgBenchConn *conn = pg_malloc0_object(PgBenchConn);

	initPQExpBuffer(&conn->err_buf);
	spanner_parse_conn_params(conn, host, port, user, dbname);
	conn->http_sock = -1;
	conn->wakeup_fd = -1;
	conn->wakeup_write_fd = -1;

#if defined(__linux__)
	conn->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
	if (conn->wakeup_fd < 0)
	{
		int			pfd[2];

		if (pipe(pfd) == 0)
		{
			fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
			fcntl(pfd[1], F_SETFL, fcntl(pfd[1], F_GETFL, 0) | O_NONBLOCK);
			conn->wakeup_fd = pfd[0];
			conn->wakeup_write_fd = pfd[1];
		}
	}

	pthread_mutex_init(&conn->mu, NULL);
	pthread_cond_init(&conn->cv, NULL);
	if (pthread_create(&conn->worker_tid, NULL, spanner_worker_main, conn) == 0)
		conn->worker_running = true;

	return conn;
}

static PgBenchConn *
spanner_connect_sync(const char *host, const char *port,
					 const char *user, const char *dbname)
{
	PgBenchConn *conn = spanner_alloc_conn(host, port, user, dbname);

	if (!spanner_ensure_session(conn))
	{
		pg_log_error("%s", conn->err_buf.data);
		active_driver->finish(conn);
		return NULL;
	}
	conn->conn_status = PGBENCH_CONN_OK;
	return conn;
}

static PgBenchConn *
spanner_connect_start(const char *host, const char *port,
					  const char *user, const char *dbname)
{
	PgBenchConn *conn = spanner_alloc_conn(host, port, user, dbname);
	SpannerTask *task = pg_malloc0_object(SpannerTask);

	conn->conn_status = PGBENCH_CONN_POLLING_READING;
	task->kind = SPANNER_TASK_CONNECT;
	spanner_enqueue_task(conn, task);
	return conn;
}

static PgBenchConnStatus
spanner_connect_poll(PgBenchConn *conn)
{
	PgBenchConnStatus st;

	if (conn == NULL)
		return PGBENCH_CONN_POLLING_FAILED;

	spanner_drain_wakeup(conn);
	pthread_mutex_lock(&conn->mu);
	if (conn->conn_status == PGBENCH_CONN_OK)
		st = PGBENCH_CONN_POLLING_OK;
	else if (conn->conn_status == PGBENCH_CONN_POLLING_FAILED)
		st = PGBENCH_CONN_POLLING_FAILED;
	else
		st = PGBENCH_CONN_POLLING_READING;
	pthread_mutex_unlock(&conn->mu);
	return st;
}

static void
spanner_finish(PgBenchConn *conn)
{
	if (conn == NULL)
		return;

	if (conn->worker_running)
	{
		pthread_mutex_lock(&conn->mu);
		conn->worker_stop = true;
		pthread_cond_broadcast(&conn->cv);
		pthread_mutex_unlock(&conn->mu);
		pthread_join(conn->worker_tid, NULL);
		conn->worker_running = false;
	}

	spanner_rpc_delete_session(conn);

	spanner_disconnect_tcp(conn);
#ifdef USE_OPENSSL
	if (conn->ssl_ctx != NULL)
	{
		SSL_CTX_free(conn->ssl_ctx);
		conn->ssl_ctx = NULL;
	}
#endif
	if (conn->wakeup_fd >= 0)
		close(conn->wakeup_fd);
	if (conn->wakeup_write_fd >= 0)
		close(conn->wakeup_write_fd);

	while (conn->res_head != NULL)
	{
		PgBenchResult *r = conn->res_head;

		conn->res_head = r->next;
		spanner_free_result(r);
	}

	while (conn->prep_head != NULL)
	{
		SpannerPreparedStmt *ps = conn->prep_head;

		conn->prep_head = ps->next;
		pg_free(ps->name);
		pg_free(ps->sql);
		pg_free(ps);
	}

	for (int i = 0; i < conn->pipe_batch_count; i++)
	{
		pg_free(conn->pipe_batch[i].sql);
		pg_free(conn->pipe_batch[i].params_json);
		pg_free(conn->pipe_batch[i].param_types_json);
	}
	pg_free(conn->pipe_batch);

	pthread_mutex_destroy(&conn->mu);
	pthread_cond_destroy(&conn->cv);

	termPQExpBuffer(&conn->err_buf);
	pg_free(conn->host);
	pg_free(conn->port);
	pg_free(conn->project);
	pg_free(conn->instance);
	pg_free(conn->database);
	pg_free(conn->db_resource);
	pg_free(conn->session_name);
	pg_free(conn->auth_token);
	pg_free(conn->active_tx_id);
	pg_free(conn->precommit_token);
	pg_free(conn);
}

static PgBenchConnStatus
spanner_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_CONN_BAD;
	return (conn->conn_status == PGBENCH_CONN_OK ||
			conn->conn_status == PGBENCH_CONN_POLLING_READING) ?
		PGBENCH_CONN_OK : PGBENCH_CONN_BAD;
}

static const char *
spanner_error_message(const PgBenchConn *conn)
{
	if (conn == NULL)
		return "connection is NULL";
	return conn->err_buf.data ? conn->err_buf.data : "";
}

static int
spanner_get_poll_fd(const PgBenchConn *conn)
{
	if (conn == NULL || conn->wakeup_fd < 0)
		return PGBENCH_POLL_ERROR;
	return conn->wakeup_fd;
}

static PgBenchTxStatus
spanner_tx_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_TX_UNKNOWN;
	return conn->tx_status;
}

static const char *
spanner_get_host(const PgBenchConn *conn)
{
	return (conn && conn->host) ? conn->host : "";
}

static const char *
spanner_get_port(const PgBenchConn *conn)
{
	return (conn && conn->port) ? conn->port : "";
}

static const char *
spanner_get_db(const PgBenchConn *conn)
{
	return (conn && conn->db_resource) ? conn->db_resource : "";
}

static void
spanner_set_cancel_conn(PgBenchConn *conn)
{
	(void) conn;
}

static void
spanner_reset_cancel_conn(void)
{
}

static bool
spanner_consume_input(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	spanner_drain_wakeup(conn);
	return true;
}

static bool
spanner_is_busy(PgBenchConn *conn)
{
	bool		busy;

	if (conn == NULL)
		return false;

	pthread_mutex_lock(&conn->mu);
	if (conn->res_head != NULL || conn->awaiting_result_null)
		busy = false;
	else
		busy = (conn->tasks_in_flight > 0);
	pthread_mutex_unlock(&conn->mu);
	return busy;
}

static PgBenchResult *
spanner_get_result(PgBenchConn *conn)
{
	PgBenchResult *res = NULL;

	if (conn == NULL)
		return NULL;

	pthread_mutex_lock(&conn->mu);
	if (conn->awaiting_result_null)
	{
		conn->awaiting_result_null = false;
		pthread_mutex_unlock(&conn->mu);
		return NULL;
	}

	while (conn->res_head == NULL && conn->tasks_in_flight > 0)
		pthread_cond_wait(&conn->cv, &conn->mu);

	if (conn->res_head != NULL)
	{
		res = conn->res_head;
		conn->res_head = res->next;
		if (conn->res_head == NULL)
			conn->res_tail = NULL;
		res->next = NULL;

		if (res->status != PGBENCH_RES_PIPELINE_SYNC)
			conn->awaiting_result_null = true;
	}
	pthread_mutex_unlock(&conn->mu);
	spanner_drain_wakeup(conn);
	return res;
}

static PgBenchResult *
spanner_exec_sync(PgBenchConn *conn, const char *sql)
{
	SpannerTask *task = pg_malloc0_object(SpannerTask);
	PgBenchResult *res;
	PgBenchResult *tmp;

	task->kind = SPANNER_TASK_QUERY;
	task->sql = pg_strdup(sql);
	spanner_enqueue_task(conn, task);

	res = spanner_get_result(conn);
	while ((tmp = spanner_get_result(conn)) != NULL)
	{
		spanner_free_result(res);
		res = tmp;
	}
	return res;
}

static void
spanner_register_prepared(PgBenchConn *conn, const char *stmt_name,
						  const char *sql, int nparams)
{
	SpannerPreparedStmt *ps;

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
	ps = pg_malloc0_object(SpannerPreparedStmt);
	ps->name = pg_strdup(stmt_name);
	ps->sql = pg_strdup(sql);
	ps->nparams = nparams;
	ps->next = conn->prep_head;
	conn->prep_head = ps;
}

static SpannerPreparedStmt *
spanner_find_prepared(PgBenchConn *conn, const char *stmt_name)
{
	for (SpannerPreparedStmt *ps = conn->prep_head; ps != NULL; ps = ps->next)
	{
		if (strcmp(ps->name, stmt_name) == 0)
			return ps;
	}
	return NULL;
}

static PgBenchResult *
spanner_prepare_sync(PgBenchConn *conn, const char *stmt_name,
					 const char *sql, int nparams)
{
	spanner_register_prepared(conn, stmt_name, sql, nparams);
	return spanner_make_result(PGBENCH_RES_COMMAND_OK, NULL, NULL);
}

static char *
spanner_escape_identifier(PgBenchConn *conn, const char *str, size_t len)
{
	PQExpBufferData buf;
	char		q = (conn && conn->is_pg_dialect) ? '"' : '`';

	initPQExpBuffer(&buf);
	appendPQExpBufferChar(&buf, q);
	for (size_t i = 0; i < len; i++)
	{
		if (str[i] == q)
			appendPQExpBufferChar(&buf, q);
		appendPQExpBufferChar(&buf, str[i]);
	}
	appendPQExpBufferChar(&buf, q);
	return buf.data;
}

static void
spanner_free_mem(void *ptr)
{
	pg_free(ptr);
}

static bool
spanner_is_dml_sql(const char *sql)
{
	const char *p = spanner_skip_sql_ws_and_comments(sql);

	if (p == NULL)
		return false;
	return (pg_strncasecmp(p, "INSERT", 6) == 0 ||
			pg_strncasecmp(p, "UPDATE", 6) == 0 ||
			pg_strncasecmp(p, "DELETE", 6) == 0);
}

static void
spanner_flush_pipe_batch_if_any(PgBenchConn *conn)
{
	SpannerTask *task;

	if (conn->pipe_batch_count == 0)
		return;

	task = pg_malloc0_object(SpannerTask);
	task->kind = SPANNER_TASK_BATCH_DML;
	task->batch_stmts = conn->pipe_batch;
	task->num_batch_stmts = conn->pipe_batch_count;
	conn->pipe_batch = NULL;
	conn->pipe_batch_count = 0;
	conn->pipe_batch_cap = 0;
	spanner_enqueue_task(conn, task);
}

static bool
spanner_dispatch_sql(PgBenchConn *conn, const char *sql, int nparams,
					 const char *const *param_texts,
					 const struct Variable *const *param_vars)
{
	char	   *params_json = NULL;
	char	   *types_json = NULL;

	spanner_build_params_json(nparams, param_texts, param_vars,
							  &params_json, &types_json);

	if (conn->pipe_status != PGBENCH_PIPE_OFF && spanner_is_dml_sql(sql))
	{
		if (conn->pipe_batch_count >= conn->pipe_batch_cap)
		{
			conn->pipe_batch_cap = (conn->pipe_batch_cap == 0) ?
				8 : (conn->pipe_batch_cap * 2);
			conn->pipe_batch = pg_realloc_array(conn->pipe_batch,
												SpannerBatchStmt,
												conn->pipe_batch_cap);
		}
		conn->pipe_batch[conn->pipe_batch_count].sql = pg_strdup(sql);
		conn->pipe_batch[conn->pipe_batch_count].params_json = params_json;
		conn->pipe_batch[conn->pipe_batch_count].param_types_json = types_json;
		conn->pipe_batch_count++;
		return true;
	}

	if (conn->pipe_batch_count > 0)
		spanner_flush_pipe_batch_if_any(conn);

	{
		SpannerTask *task = pg_malloc0_object(SpannerTask);

		task->kind = SPANNER_TASK_QUERY;
		task->sql = pg_strdup(sql);
		task->params_json = params_json;
		task->param_types_json = types_json;
		spanner_enqueue_task(conn, task);
	}
	return true;
}

static bool
spanner_send_query(PgBenchConn *conn, const char *sql)
{
	return spanner_dispatch_sql(conn, sql, 0, NULL, NULL);
}

static bool
spanner_send_query_params(PgBenchConn *conn, const char *sql, int nparams,
						  const char *const *param_texts,
						  const struct Variable *const *param_vars)
{
	return spanner_dispatch_sql(conn, sql, nparams, param_texts, param_vars);
}

static bool
spanner_send_prepare(PgBenchConn *conn, const char *stmt_name,
					 const char *sql, int nparams)
{
	SpannerTask *task;

	spanner_register_prepared(conn, stmt_name, sql, nparams);
	if (conn->pipe_batch_count > 0)
		spanner_flush_pipe_batch_if_any(conn);
	task = pg_malloc0_object(SpannerTask);
	task->kind = SPANNER_TASK_PREPARE;
	spanner_enqueue_task(conn, task);
	return true;
}

static bool
spanner_send_query_prepared(PgBenchConn *conn, const char *stmt_name, int nparams,
							const char *const *param_texts,
							const struct Variable *const *param_vars)
{
	SpannerPreparedStmt *ps = spanner_find_prepared(conn, stmt_name);

	if (ps == NULL)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "prepared statement \"%s\" does not exist\n",
						  stmt_name ? stmt_name : "");
		return false;
	}
	return spanner_dispatch_sql(conn, ps->sql, nparams, param_texts, param_vars);
}

static bool
spanner_enter_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	pthread_mutex_lock(&conn->mu);
	conn->pipe_status = PGBENCH_PIPE_ON;
	conn->pipe_aborted = false;
	conn->awaiting_result_null = false;
	pthread_mutex_unlock(&conn->mu);
	return true;
}

static bool
spanner_exit_pipeline_mode(PgBenchConn *conn)
{
	bool		ok = true;

	if (conn == NULL)
		return false;

	pthread_mutex_lock(&conn->mu);
	if (conn->tasks_in_flight > 0 || conn->res_head != NULL ||
		conn->pipe_batch_count > 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "cannot exit pipeline mode with uncollected results\n");
		ok = false;
	}
	else
	{
		conn->pipe_status = PGBENCH_PIPE_OFF;
		conn->pipe_aborted = false;
		conn->awaiting_result_null = false;
	}
	pthread_mutex_unlock(&conn->mu);
	return ok;
}

static PgBenchPipelineStatus
spanner_pipeline_status(const PgBenchConn *conn)
{
	return conn ? conn->pipe_status : PGBENCH_PIPE_OFF;
}

static bool
spanner_send_pipeline_sync(PgBenchConn *conn)
{
	SpannerTask *task;

	if (conn == NULL || conn->pipe_status == PGBENCH_PIPE_OFF)
		return false;

	spanner_flush_pipe_batch_if_any(conn);
	task = pg_malloc0_object(SpannerTask);
	task->kind = SPANNER_TASK_PIPELINE_SYNC;
	spanner_enqueue_task(conn, task);
	return true;
}

static bool
spanner_pipeline_sync(PgBenchConn *conn)
{
	return spanner_send_pipeline_sync(conn);
}

static PgBenchExecStatus
spanner_result_status(const PgBenchResult *res)
{
	return res ? res->status : PGBENCH_RES_FATAL_ERROR;
}

static const char *
spanner_result_error_field(const PgBenchResult *res, int fieldcode)
{
	if (res == NULL)
		return NULL;
	if (fieldcode == PG_DIAG_SQLSTATE && res->sqlstate[0] != '\0')
		return res->sqlstate;
	return NULL;
}

static const char *
spanner_result_error_message(const PgBenchResult *res, const PgBenchConn *conn)
{
	if (res != NULL && res->err_msg != NULL && *res->err_msg != '\0')
		return res->err_msg;
	return spanner_error_message(conn);
}

static int
spanner_ntuples(const PgBenchResult *res)
{
	return res ? res->nrows : 0;
}

static int
spanner_nfields(const PgBenchResult *res)
{
	return res ? res->ncols : 0;
}

static const char *
spanner_fname(const PgBenchResult *res, int col)
{
	if (res == NULL || col < 0 || col >= res->ncols || res->col_names == NULL)
		return NULL;
	return res->col_names[col];
}

static int
spanner_fnumber(const PgBenchResult *res, const char *col_name)
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
spanner_get_is_null(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return true;
	return res->cells[row][col] == NULL;
}

static const char *
spanner_get_value(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return "";
	return res->cells[row][col] ? res->cells[row][col] : "";
}

static void
spanner_clear_result(PgBenchResult *res)
{
	spanner_free_result(res);
}

/* -------------------------------------------------------------------------
 * Bulk Load via Spanner Mutations (insertOrUpdate)
 * -------------------------------------------------------------------------
 */

static void
spanner_fill_default_table_cols(PgBenchCopyStream *stream)
{
	static const char *const branches_cols[] = {"bid", "bbalance", "filler"};
	static const char *const tellers_cols[] = {"tid", "bid", "tbalance", "filler"};
	static const char *const accounts_cols[] = {"aid", "bid", "abalance", "filler"};
	static const char *const history_cols[] = {"tid", "bid", "aid", "delta", "mtime", "filler"};
	const char *const *src = NULL;
	int			n = 0;

	if (pg_strcasecmp(stream->table_name, "pgbench_branches") == 0)
	{
		src = branches_cols;
		n = 3;
	}
	else if (pg_strcasecmp(stream->table_name, "pgbench_tellers") == 0)
	{
		src = tellers_cols;
		n = 4;
	}
	else if (pg_strcasecmp(stream->table_name, "pgbench_accounts") == 0)
	{
		src = accounts_cols;
		n = 4;
	}
	else if (pg_strcasecmp(stream->table_name, "pgbench_history") == 0)
	{
		src = history_cols;
		n = 6;
	}

	if (src != NULL)
	{
		stream->ncols = n;
		stream->col_names = pg_malloc0_array(char *, n);
		for (int i = 0; i < n; i++)
			stream->col_names[i] = pg_strdup(src[i]);
	}
}

static bool
spanner_flush_mutation_batch(PgBenchCopyStream *stream)
{
	PQExpBufferData path;
	PQExpBufferData body;
	int			http_status = 0;
	char	   *resp = NULL;
	size_t		rlen = 0;
	bool		ok = true;

	if (stream->batch_rows == 0)
		return true;

	if (!spanner_ensure_session(stream->conn))
	{
		stream->failed = true;
		return false;
	}

	initPQExpBuffer(&path);
	printfPQExpBuffer(&path, "/v1/%s:commit", stream->conn->session_name);

	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body,
						 "{\"singleUseTransaction\": {\"readWrite\": {}}, "
						 "\"mutations\": [{\"insertOrUpdate\": {\"table\": ");
	spanner_append_json_escaped(&body, stream->table_name);
	appendPQExpBufferStr(&body, ", \"columns\": [");
	for (int i = 0; i < stream->ncols; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(&body, ", ");
		spanner_append_json_escaped(&body,
									(stream->col_names && stream->col_names[i]) ?
									stream->col_names[i] : "col");
	}
	appendPQExpBuffer(&body, "], \"values\": [%s]}}]}", stream->values_json.data);

	if (!spanner_http_rpc(stream->conn, "POST", path.data, body.data,
						  &http_status, &resp, &rlen) ||
		http_status >= 400)
	{
		if (http_status >= 400 && resp != NULL)
		{
			PgBenchResult *er = spanner_parse_error_response(http_status, resp, rlen);

			if (er != NULL)
			{
				if (er->err_msg != NULL && *er->err_msg != '\0')
					printfPQExpBuffer(&stream->conn->err_buf, "%s", er->err_msg);
				spanner_free_result(er);
			}
		}
		ok = false;
		stream->failed = true;
	}

	pg_free(resp);
	termPQExpBuffer(&path);
	termPQExpBuffer(&body);
	resetPQExpBuffer(&stream->values_json);
	stream->batch_rows = 0;
	return ok;
}

static bool
spanner_append_tsv_mutation_row(PgBenchCopyStream *stream,
								const char *line, size_t len)
{
	const char *p = line;
	const char *end = line + len;
	int			cidx = 0;
	int			max_batch_rows;

	if (len > 0 && end[-1] == '\r')
		end--;
	if (p == end)
		return true;

	if (stream->batch_rows > 0)
		appendPQExpBufferStr(&stream->values_json, ", ");
	appendPQExpBufferChar(&stream->values_json, '[');

	for (const char *c = p; c <= end; c++)
	{
		if (c == end || *c == '\t')
		{
			size_t		clen = c - p;

			if (cidx > 0)
				appendPQExpBufferStr(&stream->values_json, ", ");

			if (clen == 2 && p[0] == '\\' && p[1] == 'N')
			{
				appendPQExpBufferStr(&stream->values_json, "null");
			}
			else
			{
				char	   *val = pnstrdup(p, clen);

				spanner_append_json_escaped(&stream->values_json, val);
				pg_free(val);
			}
			cidx++;
			p = c + 1;
		}
	}
	appendPQExpBufferChar(&stream->values_json, ']');
	stream->batch_rows++;

	max_batch_rows = (stream->ncols > 0) ? (40000 / stream->ncols) : 1000;
	if (max_batch_rows < 100)
		max_batch_rows = 100;
	else if (max_batch_rows > 5000)
		max_batch_rows = 5000;

	if (stream->batch_rows >= max_batch_rows || stream->values_json.len >= 2097152)
		return spanner_flush_mutation_batch(stream);
	return true;
}

static PgBenchCopyStream *
spanner_copy_begin(PgBenchConn *conn, const char *table_name,
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
	else
	{
		spanner_fill_default_table_cols(stream);
	}
	initPQExpBuffer(&stream->line_buf);
	initPQExpBuffer(&stream->values_json);
	return stream;
}

static bool
spanner_copy_put_data(PgBenchCopyStream *stream, const char *buffer, int nbytes)
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
		if (!spanner_append_tsv_mutation_row(stream, p, nl - p))
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
spanner_copy_put_row(PgBenchCopyStream *stream, int ncols,
					 const struct PgBenchValue *cols)
{
	int			max_batch_rows;

	if (stream == NULL || stream->failed)
		return false;

	if (stream->batch_rows > 0)
		appendPQExpBufferStr(&stream->values_json, ", ");
	appendPQExpBufferChar(&stream->values_json, '[');

	for (int i = 0; i < ncols; i++)
	{
		char		numbuf[64];

		if (i > 0)
			appendPQExpBufferStr(&stream->values_json, ", ");
		switch (cols[i].type)
		{
			case PGBT_NULL:
				appendPQExpBufferStr(&stream->values_json, "null");
				break;
			case PGBT_INT:
				snprintf(numbuf, sizeof(numbuf), INT64_FORMAT, cols[i].u.ival);
				spanner_append_json_escaped(&stream->values_json, numbuf);
				break;
			case PGBT_DOUBLE:
				appendPQExpBuffer(&stream->values_json, "%.17g", cols[i].u.dval);
				break;
			case PGBT_BOOLEAN:
				appendPQExpBufferStr(&stream->values_json,
									 cols[i].u.bval ? "true" : "false");
				break;
			case PGBT_TEXT:
				spanner_append_json_escaped(&stream->values_json,
											cols[i].u.sval ? cols[i].u.sval : "");
				break;
			default:
				appendPQExpBufferStr(&stream->values_json, "null");
				break;
		}
	}
	appendPQExpBufferChar(&stream->values_json, ']');
	stream->batch_rows++;

	max_batch_rows = (stream->ncols > 0) ? (40000 / stream->ncols) : 1000;
	if (max_batch_rows < 100)
		max_batch_rows = 100;
	else if (max_batch_rows > 5000)
		max_batch_rows = 5000;

	if (stream->batch_rows >= max_batch_rows || stream->values_json.len >= 2097152)
		return spanner_flush_mutation_batch(stream);
	return true;
}

static bool
spanner_copy_end(PgBenchCopyStream *stream, const char *errormsg)
{
	bool		ok = true;

	if (stream == NULL)
		return false;

	if (errormsg == NULL && !stream->failed)
	{
		if (stream->line_buf.len > 0)
			ok = spanner_append_tsv_mutation_row(stream,
												 stream->line_buf.data,
												 stream->line_buf.len);
		if (ok)
			ok = spanner_flush_mutation_batch(stream);
	}
	else
		ok = false;

	termPQExpBuffer(&stream->line_buf);
	termPQExpBuffer(&stream->values_json);
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

PgBenchDbDriver spanner_driver = {
	.name = "spanner",
	.caps = {
		.supports_pipeline = true,
		.supports_async_connect = true,
		.supports_vacuum = false,
		.supports_pg_catalog = false,
		.supports_copy_text = true,
		.supports_typed_copy = true,
		.prefer_prepared_mode = true,
		.param_style = PGBENCH_PARAM_AT_P
	},
	.init_global = spanner_init_global,
	.shutdown_global = spanner_shutdown_global,
	.connect_sync = spanner_connect_sync,
	.connect_start = spanner_connect_start,
	.connect_poll = spanner_connect_poll,
	.finish = spanner_finish,
	.status = spanner_status,
	.error_message = spanner_error_message,
	.get_poll_fd = spanner_get_poll_fd,
	.tx_status = spanner_tx_status,
	.get_host = spanner_get_host,
	.get_port = spanner_get_port,
	.get_db = spanner_get_db,
	.set_cancel_conn = spanner_set_cancel_conn,
	.reset_cancel_conn = spanner_reset_cancel_conn,
	.exec_sync = spanner_exec_sync,
	.prepare_sync = spanner_prepare_sync,
	.escape_identifier = spanner_escape_identifier,
	.free_mem = spanner_free_mem,
	.send_query = spanner_send_query,
	.send_query_params = spanner_send_query_params,
	.send_prepare = spanner_send_prepare,
	.send_query_prepared = spanner_send_query_prepared,
	.consume_input = spanner_consume_input,
	.is_busy = spanner_is_busy,
	.get_result = spanner_get_result,
	.enter_pipeline_mode = spanner_enter_pipeline_mode,
	.exit_pipeline_mode = spanner_exit_pipeline_mode,
	.pipeline_status = spanner_pipeline_status,
	.pipeline_sync = spanner_pipeline_sync,
	.send_pipeline_sync = spanner_send_pipeline_sync,
	.result_status = spanner_result_status,
	.result_error_field = spanner_result_error_field,
	.result_error_message = spanner_result_error_message,
	.ntuples = spanner_ntuples,
	.nfields = spanner_nfields,
	.fname = spanner_fname,
	.fnumber = spanner_fnumber,
	.get_is_null = spanner_get_is_null,
	.get_value = spanner_get_value,
	.clear_result = spanner_clear_result,
	.copy_begin = spanner_copy_begin,
	.copy_put_data = spanner_copy_put_data,
	.copy_put_row = spanner_copy_put_row,
	.copy_end = spanner_copy_end
};
