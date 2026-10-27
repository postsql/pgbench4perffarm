/*-------------------------------------------------------------------------
 *
 * db_driver.h
 *		Database driver vtable abstraction for pgbench (PostgreSQL libpq,
 *		Cloud Spanner, and Remote DuckDB over the Quack protocol)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/db_driver.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_DB_DRIVER_H
#define PGBENCH_DB_DRIVER_H

#include <stddef.h>

struct Variable;
struct PgBenchValue;

/*
 * Opaque connection, result, and bulk-load handles
 */
typedef struct PgBenchConn PgBenchConn;
typedef struct PgBenchResult PgBenchResult;
typedef struct PgBenchCopyStream PgBenchCopyStream;

/*
 * Special return values for driver->get_poll_fd(conn)
 */
#define PGBENCH_POLL_ERROR	(-1)
#define PGBENCH_POLL_TASK	(-2)

/*
 * Connection status codes
 */
typedef enum PgBenchConnStatus
{
	PGBENCH_CONN_OK = 0,
	PGBENCH_CONN_BAD,
	PGBENCH_CONN_POLLING_READING,
	PGBENCH_CONN_POLLING_WRITING,
	PGBENCH_CONN_POLLING_FAILED,
	PGBENCH_CONN_POLLING_OK
} PgBenchConnStatus;

/*
 * Statement / result execution status codes
 */
typedef enum PgBenchExecStatus
{
	PGBENCH_RES_EMPTY_QUERY = 0,
	PGBENCH_RES_COMMAND_OK,
	PGBENCH_RES_TUPLES_OK,
	PGBENCH_RES_COPY_OUT,
	PGBENCH_RES_COPY_IN,
	PGBENCH_RES_BAD_RESPONSE,
	PGBENCH_RES_NONFATAL_ERROR,
	PGBENCH_RES_FATAL_ERROR,
	PGBENCH_RES_PIPELINE_SYNC,
	PGBENCH_RES_PIPELINE_ABORTED
} PgBenchExecStatus;

/*
 * Transaction state codes
 */
typedef enum PgBenchTxStatus
{
	PGBENCH_TX_IDLE = 0,
	PGBENCH_TX_ACTIVE,
	PGBENCH_TX_INTRANS,
	PGBENCH_TX_INERROR,
	PGBENCH_TX_UNKNOWN
} PgBenchTxStatus;

/*
 * Pipeline state codes
 */
typedef enum PgBenchPipelineStatus
{
	PGBENCH_PIPE_OFF = 0,
	PGBENCH_PIPE_ON,
	PGBENCH_PIPE_ABORTED
} PgBenchPipelineStatus;

/*
 * Query parameter placeholder syntax
 */
typedef enum PgBenchParamStyle
{
	PGBENCH_PARAM_DOLLAR = 0,	/* $1, $2, ... (PostgreSQL, Spanner PG, DuckDB) */
	PGBENCH_PARAM_AT_P,			/* @p1, @p2, ... (Spanner GoogleSQL) */
	PGBENCH_PARAM_QMARK			/* ?, ?, ... (Positional) */
} PgBenchParamStyle;

/*
 * Driver capability flags
 */
typedef struct PgBenchDriverCaps
{
	bool				supports_pipeline;
	bool				supports_async_connect;
	bool				supports_vacuum;
	bool				supports_pg_catalog;
	bool				supports_copy_text;
	bool				supports_typed_copy;
	bool				prefer_prepared_mode;
	PgBenchParamStyle	param_style;
} PgBenchDriverCaps;

/*
 * Database driver vtable
 */
typedef struct PgBenchDbDriver
{
	const char		   *name;		/* "postgres", "spanner", "quack" */
	PgBenchDriverCaps	caps;

	/* Process-wide initialization & teardown */
	bool				(*init_global) (const char *conninfo, int nthreads, int nclients);
	void				(*shutdown_global) (void);

	/* Connection lifecycle */
	PgBenchConn		   *(*connect_sync) (const char *host, const char *port,
										 const char *user, const char *dbname);
	PgBenchConn		   *(*connect_start) (const char *host, const char *port,
										  const char *user, const char *dbname);
	PgBenchConnStatus	(*connect_poll) (PgBenchConn *conn);
	void				(*finish) (PgBenchConn *conn);
	PgBenchConnStatus	(*status) (const PgBenchConn *conn);
	const char		   *(*error_message) (const PgBenchConn *conn);
	int					(*get_poll_fd) (const PgBenchConn *conn);
	PgBenchTxStatus		(*tx_status) (const PgBenchConn *conn);
	const char		   *(*get_host) (const PgBenchConn *conn);
	const char		   *(*get_port) (const PgBenchConn *conn);
	const char		   *(*get_db) (const PgBenchConn *conn);

	/* Signal cancellation hook (for init.c) */
	void				(*set_cancel_conn) (PgBenchConn *conn);
	void				(*reset_cancel_conn) (void);

	/* Synchronous execution (setup, init, printVersion, prepare) */
	PgBenchResult	   *(*exec_sync) (PgBenchConn *conn, const char *sql);
	PgBenchResult	   *(*prepare_sync) (PgBenchConn *conn,
										 const char *stmt_name,
										 const char *sql,
										 int nparams);
	char			   *(*escape_identifier) (PgBenchConn *conn,
											  const char *str,
											  size_t len);
	void				(*free_mem) (void *ptr);

	/* Asynchronous command dispatch */
	bool				(*send_query) (PgBenchConn *conn, const char *sql);
	bool				(*send_query_params) (PgBenchConn *conn,
											  const char *sql,
											  int nparams,
											  const char *const *param_texts,
											  const struct Variable *const *param_vars);
	bool				(*send_prepare) (PgBenchConn *conn,
										 const char *stmt_name,
										 const char *sql,
										 int nparams);
	bool				(*send_query_prepared) (PgBenchConn *conn,
												const char *stmt_name,
												int nparams,
												const char *const *param_texts,
												const struct Variable *const *param_vars);

	/* Asynchronous result polling */
	bool				(*consume_input) (PgBenchConn *conn);
	bool				(*is_busy) (PgBenchConn *conn);
	PgBenchResult	   *(*get_result) (PgBenchConn *conn);

	/* Pipeline / batch mode */
	bool				(*enter_pipeline_mode) (PgBenchConn *conn);
	bool				(*exit_pipeline_mode) (PgBenchConn *conn);
	PgBenchPipelineStatus (*pipeline_status) (const PgBenchConn *conn);
	bool				(*pipeline_sync) (PgBenchConn *conn);
	bool				(*send_pipeline_sync) (PgBenchConn *conn);

	/* Result inspection & cleanup */
	PgBenchExecStatus	(*result_status) (const PgBenchResult *res);
	const char		   *(*result_error_field) (const PgBenchResult *res, int fieldcode);
	const char		   *(*result_error_message) (const PgBenchResult *res,
												 const PgBenchConn *conn);
	int					(*ntuples) (const PgBenchResult *res);
	int					(*nfields) (const PgBenchResult *res);
	const char		   *(*fname) (const PgBenchResult *res, int col);
	int					(*fnumber) (const PgBenchResult *res, const char *col_name);
	bool				(*get_is_null) (const PgBenchResult *res, int row, int col);
	const char		   *(*get_value) (const PgBenchResult *res, int row, int col);
	void				(*clear_result) (PgBenchResult *res);

	/* Bulk data loading (COPY / Spanner Mutations / Quack Bulk Insert) */
	PgBenchCopyStream  *(*copy_begin) (PgBenchConn *conn,
									   const char *table_name,
									   int ncols,
									   const char *const *col_names,
									   const char *copy_sql);
	bool				(*copy_put_data) (PgBenchCopyStream *stream,
										  const char *buffer,
										  int nbytes);
	bool				(*copy_put_row) (PgBenchCopyStream *stream,
										 int ncols,
										 const struct PgBenchValue *cols);
	bool				(*copy_end) (PgBenchCopyStream *stream,
									 const char *errormsg);
} PgBenchDbDriver;

extern const PgBenchDbDriver postgres_driver;
extern PgBenchDbDriver spanner_driver;
extern const PgBenchDbDriver quack_driver;
extern const PgBenchDbDriver mysql_driver;
extern const PgBenchDbDriver neo4j_driver;

extern const PgBenchDbDriver *active_driver;
extern bool driver_explicitly_set;
extern char *driver_dialect;

extern void setActiveDriverByName(const char *driver_name);
extern void setDriverDialect(const char *dialect_name);
extern bool detectDriverFromConnStr(const char *conn_str);

#endif							/* PGBENCH_DB_DRIVER_H */
