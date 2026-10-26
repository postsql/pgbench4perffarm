/*-------------------------------------------------------------------------
 *
 * postgres_driver.c
 *		PostgreSQL (libpq) implementation of the PgBenchDbDriver vtable
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/postgres_driver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "common/string.h"
#include "db_driver.h"
#include "engine.h"
#include "fe_utils/cancel.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "variable.h"

const PgBenchDbDriver *active_driver = &postgres_driver;
bool		driver_explicitly_set = false;
char	   *driver_dialect = NULL;

void
setActiveDriverByName(const char *driver_name)
{
	if (driver_name == NULL || *driver_name == '\0')
		pg_fatal("driver name must not be empty");

	if (pg_strcasecmp(driver_name, "postgres") == 0 ||
		pg_strcasecmp(driver_name, "postgresql") == 0 ||
		pg_strcasecmp(driver_name, "pg") == 0 ||
		pg_strcasecmp(driver_name, "libpq") == 0)
	{
		active_driver = &postgres_driver;
	}
	else if (pg_strcasecmp(driver_name, "spanner") == 0 ||
			 pg_strcasecmp(driver_name, "cloud-spanner") == 0)
	{
		active_driver = &spanner_driver;
	}
	else if (pg_strcasecmp(driver_name, "quack") == 0 ||
			 pg_strcasecmp(driver_name, "duckdb") == 0)
	{
		active_driver = &quack_driver;
	}
	else
	{
		pg_fatal("invalid database driver \"%s\" (expected \"postgres\", \"spanner\", or \"quack\")",
				 driver_name);
	}
	driver_explicitly_set = true;
}

void
setDriverDialect(const char *dialect_name)
{
	if (dialect_name == NULL || *dialect_name == '\0')
		pg_fatal("dialect name must not be empty");

	if (pg_strcasecmp(dialect_name, "googlesql") != 0 &&
		pg_strcasecmp(dialect_name, "postgresql") != 0 &&
		pg_strcasecmp(dialect_name, "postgres") != 0 &&
		pg_strcasecmp(dialect_name, "pg") != 0)
	{
		pg_fatal("invalid dialect \"%s\" (expected \"googlesql\" or \"postgresql\")",
				 dialect_name);
	}

	pg_free(driver_dialect);
	if (pg_strcasecmp(dialect_name, "googlesql") == 0)
	{
		driver_dialect = pg_strdup("googlesql");
		spanner_driver.caps.param_style = PGBENCH_PARAM_AT_P;
	}
	else
	{
		driver_dialect = pg_strdup("postgresql");
		spanner_driver.caps.param_style = PGBENCH_PARAM_DOLLAR;
	}
}

bool
detectDriverFromConnStr(const char *conn_str)
{
	if (conn_str == NULL || *conn_str == '\0')
		return false;

	/* Also inspect dialect=... query param if present in URI */
	if (driver_dialect == NULL)
	{
		const char *dp = strstr(conn_str, "dialect=");

		if (dp != NULL)
		{
			dp += 8;
			if (pg_strncasecmp(dp, "postgresql", 10) == 0 ||
				pg_strncasecmp(dp, "postgres", 8) == 0 ||
				pg_strncasecmp(dp, "pg", 2) == 0)
				setDriverDialect("postgresql");
			else if (pg_strncasecmp(dp, "googlesql", 9) == 0)
				setDriverDialect("googlesql");
		}
	}

	if (driver_explicitly_set)
		return false;

	if (pg_strncasecmp(conn_str, "spanner://", 10) == 0 ||
		pg_strncasecmp(conn_str, "projects/", 9) == 0)
	{
		active_driver = &spanner_driver;
		return true;
	}
	if (pg_strncasecmp(conn_str, "quack://", 8) == 0 ||
		pg_strncasecmp(conn_str, "duckdb://", 9) == 0)
	{
		active_driver = &quack_driver;
		return true;
	}
	if (pg_strncasecmp(conn_str, "postgres://", 11) == 0 ||
		pg_strncasecmp(conn_str, "postgresql://", 13) == 0)
	{
		active_driver = &postgres_driver;
		return true;
	}
	return false;
}

/* -------------------------------------------------------------------------
 * PostgreSQL (libpq) Driver Implementation
 * -------------------------------------------------------------------------
 */

static bool
pg_init_global(const char *conninfo, int nthreads_arg, int nclients_arg)
{
	(void) conninfo;
	(void) nthreads_arg;
	(void) nclients_arg;
	return true;
}

static void
pg_shutdown_global(void)
{
}

#define PG_PARAMS_ARRAY_SIZE 8

static int	pg11_supports_call = -1;

static bool
pg_conn_supports_call(PGconn *conn)
{
	int			sver;

	if (conn == NULL)
		return true;
	sver = PQserverVersion(conn);
	if (sver >= 120000)
		return true;
	if (sver > 0 && sver < 110000)
		return false;
	if (sver >= 110000 && pg11_supports_call >= 0)
		return pg11_supports_call != 0;
	return true;
}

static PgBenchConn *
pg_connect_sync(const char *host, const char *port,
				const char *user, const char *dbname)
{
	PGconn	   *conn;
	bool		new_pass;

	do
	{
		const char *keywords[PG_PARAMS_ARRAY_SIZE];
		const char *values[PG_PARAMS_ARRAY_SIZE];
		int			k = 0;

		keywords[k] = "host";
		values[k++] = host;
		if (resolved_hostaddr != NULL && host == pghost)
		{
			keywords[k] = "hostaddr";
			values[k++] = resolved_hostaddr;
		}
		keywords[k] = "port";
		values[k++] = port;
		keywords[k] = "user";
		values[k++] = user;
		keywords[k] = "password";
		values[k++] = saved_password;
		keywords[k] = "dbname";
		values[k++] = dbname;
		keywords[k] = "fallback_application_name";
		values[k++] = progname;
		keywords[k] = NULL;
		values[k] = NULL;

		new_pass = false;

		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
		{
			pg_log_error("connection to database \"%s\" failed",
						 dbname ? dbname : "");
			return NULL;
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!saved_password)
		{
			PQfinish(conn);
			saved_password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	if (pg11_supports_call < 0)
	{
		int			sver = PQserverVersion(conn);

		if (sver >= 110000 && sver < 120000)
		{
			PGresult   *res = PQexec(conn,
									 "SELECT 1 FROM pg_attribute "
									 "WHERE attrelid = 'pg_proc'::regclass "
									 "AND attname = 'prokind'");

			pg11_supports_call = (res != NULL &&
								  PQresultStatus(res) == PGRES_TUPLES_OK &&
								  PQntuples(res) > 0) ? 1 : 0;
			PQclear(res);
		}
	}

	return (PgBenchConn *) conn;
}

static PgBenchConn *
pg_connect_start(const char *host, const char *port,
				 const char *user, const char *dbname)
{
	PGconn	   *conn;
	const char *keywords[PG_PARAMS_ARRAY_SIZE];
	const char *values[PG_PARAMS_ARRAY_SIZE];
	int			k = 0;

	keywords[k] = "host";
	values[k++] = host;
	if (resolved_hostaddr != NULL && host == pghost)
	{
		keywords[k] = "hostaddr";
		values[k++] = resolved_hostaddr;
	}
	keywords[k] = "port";
	values[k++] = port;
	keywords[k] = "user";
	values[k++] = user;
	keywords[k] = "password";
	values[k++] = saved_password;
	keywords[k] = "dbname";
	values[k++] = dbname;
	keywords[k] = "fallback_application_name";
	values[k++] = progname;
	keywords[k] = NULL;
	values[k] = NULL;

	conn = PQconnectStartParams(keywords, values, true);
	if (!conn)
	{
		pg_log_error("connection to database \"%s\" failed",
					 dbname ? dbname : "");
		return NULL;
	}

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	return (PgBenchConn *) conn;
}

static PgBenchConnStatus
pg_connect_poll(PgBenchConn *conn)
{
	PostgresPollingStatusType st = PQconnectPoll((PGconn *) conn);

	switch (st)
	{
		case PGRES_POLLING_READING:
			return PGBENCH_CONN_POLLING_READING;
		case PGRES_POLLING_WRITING:
			return PGBENCH_CONN_POLLING_WRITING;
		case PGRES_POLLING_OK:
			return PGBENCH_CONN_POLLING_OK;
		case PGRES_POLLING_FAILED:
		default:
			return PGBENCH_CONN_POLLING_FAILED;
	}
}

static void
pg_finish(PgBenchConn *conn)
{
	if (conn != NULL)
		PQfinish((PGconn *) conn);
}

static PgBenchConnStatus
pg_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_CONN_BAD;
	return (PQstatus((const PGconn *) conn) == CONNECTION_OK) ?
		PGBENCH_CONN_OK : PGBENCH_CONN_BAD;
}

static const char *
pg_error_message(const PgBenchConn *conn)
{
	if (conn == NULL)
		return "connection is NULL";
	return PQerrorMessage((const PGconn *) conn);
}

static int
pg_get_poll_fd(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_POLL_ERROR;
	return PQsocket((const PGconn *) conn);
}

static PgBenchTxStatus
pg_tx_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_TX_UNKNOWN;

	switch (PQtransactionStatus((const PGconn *) conn))
	{
		case PQTRANS_IDLE:
			return PGBENCH_TX_IDLE;
		case PQTRANS_ACTIVE:
			return PGBENCH_TX_ACTIVE;
		case PQTRANS_INTRANS:
			return PGBENCH_TX_INTRANS;
		case PQTRANS_INERROR:
			return PGBENCH_TX_INERROR;
		case PQTRANS_UNKNOWN:
		default:
			return PGBENCH_TX_UNKNOWN;
	}
}

static const char *
pg_get_host(const PgBenchConn *conn)
{
	const char *h = conn ? PQhost((const PGconn *) conn) : NULL;

	return h ? h : "";
}

static const char *
pg_get_port(const PgBenchConn *conn)
{
	const char *p = conn ? PQport((const PGconn *) conn) : NULL;

	return p ? p : "";
}

static const char *
pg_get_db(const PgBenchConn *conn)
{
	const char *d = conn ? PQdb((const PGconn *) conn) : NULL;

	return d ? d : "";
}

static void
pg_set_cancel_conn(PgBenchConn *conn)
{
	if (conn != NULL)
		SetCancelConn((PGconn *) conn);
}

static void
pg_reset_cancel_conn(void)
{
	ResetCancelConn();
}

static char *
pg_maybe_rewrite_call_sql(PGconn *conn, const char *sql)
{
	const char *p;
	size_t		prefix_ws;
	size_t		rest_len;
	char	   *buf;

	if (sql == NULL || pg_conn_supports_call(conn))
		return NULL;

	p = sql;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;

	if (pg_strncasecmp(p, "CALL ", 5) != 0 &&
		pg_strncasecmp(p, "CALL\t", 5) != 0)
		return NULL;

	prefix_ws = (size_t) (p - sql);
	rest_len = strlen(p + 4);
	buf = pg_malloc(prefix_ws + 6 + rest_len + 1);
	if (prefix_ws > 0)
		memcpy(buf, sql, prefix_ws);
	memcpy(buf + prefix_ws, "SELECT", 6);
	memcpy(buf + prefix_ws + 6, p + 4, rest_len + 1);
	return buf;
}

static PgBenchResult *
pg_exec_sync(PgBenchConn *conn, const char *sql)
{
	char	   *rewritten = pg_maybe_rewrite_call_sql((PGconn *) conn, sql);
	PGresult   *res = PQexec((PGconn *) conn, rewritten ? rewritten : sql);

	pg_free(rewritten);
	return (PgBenchResult *) res;
}

static PgBenchResult *
pg_prepare_sync(PgBenchConn *conn, const char *stmt_name,
				const char *sql, int nparams)
{
	char	   *rewritten = pg_maybe_rewrite_call_sql((PGconn *) conn, sql);
	PGresult   *res = PQprepare((PGconn *) conn, stmt_name,
								rewritten ? rewritten : sql, nparams, NULL);

	pg_free(rewritten);
	return (PgBenchResult *) res;
}

static char *
pg_escape_identifier(PgBenchConn *conn, const char *str, size_t len)
{
	return PQescapeIdentifier((PGconn *) conn, str, len);
}

static void
pg_free_mem(void *ptr)
{
	PQfreemem(ptr);
}

static bool
pg_send_query(PgBenchConn *conn, const char *sql)
{
	char	   *rewritten = pg_maybe_rewrite_call_sql((PGconn *) conn, sql);
	int			rc = PQsendQuery((PGconn *) conn, rewritten ? rewritten : sql);

	pg_free(rewritten);
	return rc == 1;
}

static bool
pg_send_query_params(PgBenchConn *conn, const char *sql, int nparams,
					 const char *const *param_texts,
					 const struct Variable *const *param_vars)
{
	char	   *rewritten = pg_maybe_rewrite_call_sql((PGconn *) conn, sql);
	int			rc;

	(void) param_vars;
	rc = PQsendQueryParams((PGconn *) conn, rewritten ? rewritten : sql,
						   nparams, NULL, param_texts, NULL, NULL, 0);
	pg_free(rewritten);
	return rc == 1;
}

static bool
pg_send_prepare(PgBenchConn *conn, const char *stmt_name,
				const char *sql, int nparams)
{
	char	   *rewritten = pg_maybe_rewrite_call_sql((PGconn *) conn, sql);
	int			rc = PQsendPrepare((PGconn *) conn, stmt_name,
								   rewritten ? rewritten : sql, nparams, NULL);

	pg_free(rewritten);
	return rc == 1;
}

static bool
pg_send_query_prepared(PgBenchConn *conn, const char *stmt_name, int nparams,
					   const char *const *param_texts,
					   const struct Variable *const *param_vars)
{
	(void) param_vars;
	return PQsendQueryPrepared((PGconn *) conn, stmt_name, nparams,
							   param_texts, NULL, NULL, 0) == 1;
}

static bool
pg_consume_input(PgBenchConn *conn)
{
	return PQconsumeInput((PGconn *) conn) == 1;
}

static bool
pg_is_busy(PgBenchConn *conn)
{
	return PQisBusy((PGconn *) conn) != 0;
}

static PgBenchResult *
pg_get_result(PgBenchConn *conn)
{
	return (PgBenchResult *) PQgetResult((PGconn *) conn);
}

static bool
pg_enter_pipeline_mode(PgBenchConn *conn)
{
	return PQenterPipelineMode((PGconn *) conn) == 1;
}

static bool
pg_exit_pipeline_mode(PgBenchConn *conn)
{
	return PQexitPipelineMode((PGconn *) conn) == 1;
}

static PgBenchPipelineStatus
pg_pipeline_status(const PgBenchConn *conn)
{
	if (conn == NULL)
		return PGBENCH_PIPE_OFF;

	switch (PQpipelineStatus((const PGconn *) conn))
	{
		case PQ_PIPELINE_ON:
			return PGBENCH_PIPE_ON;
		case PQ_PIPELINE_ABORTED:
			return PGBENCH_PIPE_ABORTED;
		case PQ_PIPELINE_OFF:
		default:
			return PGBENCH_PIPE_OFF;
	}
}

static bool
pg_pipeline_sync(PgBenchConn *conn)
{
	return PQpipelineSync((PGconn *) conn) == 1;
}

static bool
pg_send_pipeline_sync(PgBenchConn *conn)
{
	return PQsendPipelineSync((PGconn *) conn) == 1;
}

static PgBenchExecStatus
pg_result_status(const PgBenchResult *res)
{
	if (res == NULL)
		return PGBENCH_RES_FATAL_ERROR;

	switch (PQresultStatus((const PGresult *) res))
	{
		case PGRES_EMPTY_QUERY:
			return PGBENCH_RES_EMPTY_QUERY;
		case PGRES_COMMAND_OK:
			return PGBENCH_RES_COMMAND_OK;
		case PGRES_TUPLES_OK:
		case PGRES_SINGLE_TUPLE:
		case PGRES_TUPLES_CHUNK:
			return PGBENCH_RES_TUPLES_OK;
		case PGRES_COPY_OUT:
			return PGBENCH_RES_COPY_OUT;
		case PGRES_COPY_IN:
			return PGBENCH_RES_COPY_IN;
		case PGRES_BAD_RESPONSE:
			return PGBENCH_RES_BAD_RESPONSE;
		case PGRES_NONFATAL_ERROR:
			return PGBENCH_RES_NONFATAL_ERROR;
		case PGRES_PIPELINE_SYNC:
			return PGBENCH_RES_PIPELINE_SYNC;
		case PGRES_PIPELINE_ABORTED:
			return PGBENCH_RES_PIPELINE_ABORTED;
		case PGRES_FATAL_ERROR:
		default:
			return PGBENCH_RES_FATAL_ERROR;
	}
}

static const char *
pg_result_error_field(const PgBenchResult *res, int fieldcode)
{
	if (res == NULL)
		return NULL;
	return PQresultErrorField((const PGresult *) res, fieldcode);
}

static const char *
pg_result_error_message(const PgBenchResult *res, const PgBenchConn *conn)
{
	const char *msg = NULL;

	if (res != NULL)
		msg = PQresultErrorMessage((const PGresult *) res);
	if ((msg == NULL || *msg == '\0') && conn != NULL)
		msg = PQerrorMessage((const PGconn *) conn);
	return msg ? msg : "";
}

static int
pg_ntuples(const PgBenchResult *res)
{
	return res ? PQntuples((const PGresult *) res) : 0;
}

static int
pg_nfields(const PgBenchResult *res)
{
	return res ? PQnfields((const PGresult *) res) : 0;
}

static const char *
pg_fname(const PgBenchResult *res, int col)
{
	return res ? PQfname((const PGresult *) res, col) : NULL;
}

static int
pg_fnumber(const PgBenchResult *res, const char *col_name)
{
	return res ? PQfnumber((const PGresult *) res, col_name) : -1;
}

static bool
pg_get_is_null(const PgBenchResult *res, int row, int col)
{
	return res ? (PQgetisnull((const PGresult *) res, row, col) != 0) : true;
}

static const char *
pg_get_value(const PgBenchResult *res, int row, int col)
{
	return res ? PQgetvalue((const PGresult *) res, row, col) : NULL;
}

static void
pg_clear_result(PgBenchResult *res)
{
	if (res != NULL)
		PQclear((PGresult *) res);
}

static PgBenchCopyStream *
pg_copy_begin(PgBenchConn *conn, const char *table_name,
			  int ncols, const char *const *col_names,
			  const char *copy_sql)
{
	PGresult   *res;
	char	   *built_sql = NULL;

	if (copy_sql == NULL)
	{
		PQExpBufferData buf;

		initPQExpBuffer(&buf);
		printfPQExpBuffer(&buf, "COPY %s", table_name);
		if (ncols > 0 && col_names != NULL)
		{
			appendPQExpBufferChar(&buf, '(');
			for (int i = 0; i < ncols; i++)
			{
				if (i > 0)
					appendPQExpBufferStr(&buf, ", ");
				appendPQExpBufferStr(&buf, col_names[i]);
			}
			appendPQExpBufferChar(&buf, ')');
		}
		appendPQExpBufferStr(&buf, " FROM STDIN");
		built_sql = buf.data;
		copy_sql = built_sql;
	}

	res = PQexec((PGconn *) conn, copy_sql);
	pg_free(built_sql);

	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		PQclear(res);
		return NULL;
	}
	PQclear(res);
	return (PgBenchCopyStream *) conn;
}

static bool
pg_copy_put_data(PgBenchCopyStream *stream, const char *buffer, int nbytes)
{
	return PQputCopyData((PGconn *) stream, buffer, nbytes) == 1;
}

static bool
pg_copy_put_row(PgBenchCopyStream *stream, int ncols,
				const struct PgBenchValue *cols)
{
	PQExpBufferData line;
	bool		ok;

	initPQExpBuffer(&line);
	for (int i = 0; i < ncols; i++)
	{
		if (i > 0)
			appendPQExpBufferChar(&line, '\t');
		switch (cols[i].type)
		{
			case PGBT_NULL:
				appendPQExpBufferStr(&line, "\\N");
				break;
			case PGBT_INT:
				appendPQExpBuffer(&line, INT64_FORMAT, cols[i].u.ival);
				break;
			case PGBT_DOUBLE:
				appendPQExpBuffer(&line, "%.15g", cols[i].u.dval);
				break;
			case PGBT_BOOLEAN:
				appendPQExpBufferStr(&line, cols[i].u.bval ? "t" : "f");
				break;
			case PGBT_TEXT:
				if (cols[i].u.sval != NULL)
					appendPQExpBufferStr(&line, cols[i].u.sval);
				break;
			default:
				appendPQExpBufferStr(&line, "\\N");
				break;
		}
	}
	appendPQExpBufferChar(&line, '\n');
	ok = pg_copy_put_data(stream, line.data, (int) line.len);
	termPQExpBuffer(&line);
	return ok;
}

static bool
pg_copy_end(PgBenchCopyStream *stream, const char *errormsg)
{
	PGconn	   *conn = (PGconn *) stream;
	PGresult   *res;
	bool		ok = true;

	if (PQputCopyEnd(conn, errormsg) != 1)
		return false;

	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			ok = false;
		PQclear(res);
	}
	return ok;
}

const PgBenchDbDriver postgres_driver = {
	.name = "postgres",
	.caps = {
		.supports_pipeline = true,
		.supports_async_connect = true,
		.supports_vacuum = true,
		.supports_pg_catalog = true,
		.supports_copy_text = true,
		.supports_typed_copy = false,
		.prefer_prepared_mode = false,
		.param_style = PGBENCH_PARAM_DOLLAR
	},
	.init_global = pg_init_global,
	.shutdown_global = pg_shutdown_global,
	.connect_sync = pg_connect_sync,
	.connect_start = pg_connect_start,
	.connect_poll = pg_connect_poll,
	.finish = pg_finish,
	.status = pg_status,
	.error_message = pg_error_message,
	.get_poll_fd = pg_get_poll_fd,
	.tx_status = pg_tx_status,
	.get_host = pg_get_host,
	.get_port = pg_get_port,
	.get_db = pg_get_db,
	.set_cancel_conn = pg_set_cancel_conn,
	.reset_cancel_conn = pg_reset_cancel_conn,
	.exec_sync = pg_exec_sync,
	.prepare_sync = pg_prepare_sync,
	.escape_identifier = pg_escape_identifier,
	.free_mem = pg_free_mem,
	.send_query = pg_send_query,
	.send_query_params = pg_send_query_params,
	.send_prepare = pg_send_prepare,
	.send_query_prepared = pg_send_query_prepared,
	.consume_input = pg_consume_input,
	.is_busy = pg_is_busy,
	.get_result = pg_get_result,
	.enter_pipeline_mode = pg_enter_pipeline_mode,
	.exit_pipeline_mode = pg_exit_pipeline_mode,
	.pipeline_status = pg_pipeline_status,
	.pipeline_sync = pg_pipeline_sync,
	.send_pipeline_sync = pg_send_pipeline_sync,
	.result_status = pg_result_status,
	.result_error_field = pg_result_error_field,
	.result_error_message = pg_result_error_message,
	.ntuples = pg_ntuples,
	.nfields = pg_nfields,
	.fname = pg_fname,
	.fnumber = pg_fnumber,
	.get_is_null = pg_get_is_null,
	.get_value = pg_get_value,
	.clear_result = pg_clear_result,
	.copy_begin = pg_copy_begin,
	.copy_put_data = pg_copy_put_data,
	.copy_put_row = pg_copy_put_row,
	.copy_end = pg_copy_end
};
