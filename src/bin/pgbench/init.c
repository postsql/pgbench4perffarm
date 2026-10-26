/*-------------------------------------------------------------------------
 *
 * init.c
 *	  Database initialization and schema setup for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <math.h>
#include <unistd.h>

#include "catalog/pg_class_d.h"
#include "common/logging.h"
#include "engine.h"
#include "fe_utils/cancel.h"
#include "fe_utils/string_utils.h"
#include "init.h"
#include "pgbench.h"
#include "script.h"
#include "stats.h"

/*
 * scaling factor. for example, scale = 10 will make 1000000 tuples in
 * pgbench_accounts table.
 */
int	scale = 1;

/*
 * fillfactor. for example, fillfactor = 90 will use only 90 percent
 * space during inserts and leave 10 percent free.
 */
int	fillfactor = 100;

/*
 * use unlogged tables?
 */
bool unlogged_tables = false;


/*
 * tablespace selection
 */
char *tablespace = NULL;
char *index_tablespace = NULL;

/*
 * Number of "pgbench_accounts" partitions.  0 is the default and means no
 * partitioning.
 */
int	partitions = 0;


partition_method_t partition_method = PART_NONE;
const char *const PARTITION_METHOD[] = {"none", "range", "hash"};

static char
get_table_relkind(PGconn *con, const char *table)
{
	PGresult   *res;
	char	   *val;
	char		relkind;
	const char *params[1] = {table};
	const char *sql =
		"SELECT relkind FROM pg_catalog.pg_class WHERE oid=$1::pg_catalog.regclass";

	res = PQexecParams(con, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	val = PQgetvalue(res, 0, 0);
	Assert(strlen(val) == 1);
	relkind = val[0];
	PQclear(res);

	return relkind;
}

/* call active_driver->exec_sync() and exit() on failure */
static void
executeStatement(PgBenchConn *con, const char *sql)
{
	PgBenchResult *res;

	res = active_driver->exec_sync(con, sql);
	if (active_driver->result_status(res) != PGBENCH_RES_COMMAND_OK &&
		active_driver->result_status(res) != PGBENCH_RES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", active_driver->error_message(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	active_driver->clear_result(res);
}

/* call active_driver->exec_sync() and complain, but without exiting, on failure */
void
tryExecuteStatement(PgBenchConn *con, const char *sql)
{
	PgBenchResult *res;

	res = active_driver->exec_sync(con, sql);
	if (active_driver->result_status(res) != PGBENCH_RES_COMMAND_OK &&
		active_driver->result_status(res) != PGBENCH_RES_TUPLES_OK)
	{
		pg_log_error("%s", active_driver->error_message(con));
		pg_log_error_detail("(ignoring this error and continuing anyway)");
	}
	active_driver->clear_result(res);
}

/*
 * Remove old pgbench tables, if any exist
 */
static void
initDropTables(PgBenchConn *con)
{
	fprintf(stderr, "dropping old tables...\n");

	if (active_driver == &postgres_driver)
	{
		/*
		 * We drop all the tables in one command, so that whether there are
		 * foreign key dependencies or not doesn't matter.
		 */
		executeStatement(con, "drop table if exists "
						 "pgbench_accounts, "
						 "pgbench_branches, "
						 "pgbench_history, "
						 "pgbench_tellers");
	}
	else
	{
		executeStatement(con, "drop table if exists pgbench_history");
		executeStatement(con, "drop table if exists pgbench_accounts");
		executeStatement(con, "drop table if exists pgbench_tellers");
		executeStatement(con, "drop table if exists pgbench_branches");
	}
}

/*
 * Create "pgbench_accounts" partitions if needed.
 *
 * This is the larger table of pgbench default tpc-b like schema
 * with a known size, so we choose to partition it.
 */
static void
createPartitions(PgBenchConn *con)
{
	PQExpBufferData query;

	/* we must have to create some partitions */
	Assert(partitions > 0);

	fprintf(stderr, "creating %d partitions...\n", partitions);

	initPQExpBuffer(&query);

	for (int p = 1; p <= partitions; p++)
	{
		if (partition_method == PART_RANGE)
		{
			int64		part_size = (naccounts * (int64) scale + partitions - 1) / partitions;

			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values from (",
							  unlogged_tables ? " unlogged" : "", p);

			/*
			 * For RANGE, we use open-ended partitions at the beginning and
			 * end to allow any valid value for the primary key.  Although the
			 * actual minimum and maximum values can be derived from the
			 * scale, it is more generic and the performance is better.
			 */
			if (p == 1)
				appendPQExpBufferStr(&query, "minvalue");
			else
				appendPQExpBuffer(&query, INT64_FORMAT, (p - 1) * part_size + 1);

			appendPQExpBufferStr(&query, ") to (");

			if (p < partitions)
				appendPQExpBuffer(&query, INT64_FORMAT, p * part_size + 1);
			else
				appendPQExpBufferStr(&query, "maxvalue");

			appendPQExpBufferChar(&query, ')');
		}
		else if (partition_method == PART_HASH)
			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values with (modulus %d, remainder %d)",
							  unlogged_tables ? " unlogged" : "", p,
							  partitions, p - 1);
		else					/* cannot get there */
			Assert(0);

		/*
		 * Per ddlinfo in initCreateTables, fillfactor is needed on table
		 * pgbench_accounts.
		 */
		if (active_driver == &postgres_driver)
			appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create pgbench's standard tables
 */
static void
initCreateTables(PgBenchConn *con)
{
	/*
	 * Note: TPC-B requires at least 100 bytes per row, and the "filler"
	 * fields in these table declarations were intended to comply with that.
	 * The pgbench_accounts table complies with that because the "filler"
	 * column is set to blank-padded empty string. But for all other tables
	 * the columns default to NULL and so don't actually take any space.  We
	 * could fix that by giving them non-null default values.  However, that
	 * would completely break comparability of pgbench results with prior
	 * versions. Since pgbench has never pretended to be fully TPC-B compliant
	 * anyway, we stick with the historical behavior.
	 */
	struct ddlinfo
	{
		const char *table;		/* table name */
		const char *smcols;		/* column decls if accountIDs are 32 bits */
		const char *bigcols;	/* column decls if accountIDs are 64 bits */
		int			declare_fillfactor;
	};
	static const struct ddlinfo DDLs[] = {
		{
			"pgbench_history",
			"tid int,bid int,aid    int,delta int,mtime timestamp,filler char(22)",
			"tid int,bid int,aid bigint,delta int,mtime timestamp,filler char(22)",
			0
		},
		{
			"pgbench_tellers",
			"tid int not null,bid int,tbalance int,filler char(84)",
			"tid int not null,bid int,tbalance int,filler char(84)",
			1
		},
		{
			"pgbench_accounts",
			"aid    int not null,bid int,abalance int,filler char(84)",
			"aid bigint not null,bid int,abalance int,filler char(84)",
			1
		},
		{
			"pgbench_branches",
			"bid int not null,bbalance int,filler char(88)",
			"bid int not null,bbalance int,filler char(88)",
			1
		}
	};
	PQExpBufferData query;

	fprintf(stderr, "creating tables...\n");

	if (active_driver == &spanner_driver)
	{
		if (active_driver->caps.param_style == PGBENCH_PARAM_AT_P)
		{
			executeStatement(con,
							 "CREATE TABLE pgbench_branches ("
							 "bid INT64 NOT NULL, bbalance INT64, filler STRING(88)"
							 ") PRIMARY KEY (bid)");
			executeStatement(con,
							 "CREATE TABLE pgbench_tellers ("
							 "tid INT64 NOT NULL, bid INT64, tbalance INT64, filler STRING(84)"
							 ") PRIMARY KEY (tid)");
			executeStatement(con,
							 "CREATE TABLE pgbench_accounts ("
							 "aid INT64 NOT NULL, bid INT64, abalance INT64, filler STRING(84)"
							 ") PRIMARY KEY (aid)");
			executeStatement(con,
							 "CREATE TABLE pgbench_history ("
							 "tid INT64, bid INT64, aid INT64, delta INT64, mtime TIMESTAMP, filler STRING(22)"
							 ") PRIMARY KEY (aid, tid, bid, mtime)");
		}
		else
		{
			executeStatement(con,
							 "CREATE TABLE pgbench_branches ("
							 "bid bigint not null primary key, bbalance bigint, filler varchar(88))");
			executeStatement(con,
							 "CREATE TABLE pgbench_tellers ("
							 "tid bigint not null primary key, bid bigint, tbalance bigint, filler varchar(84))");
			executeStatement(con,
							 "CREATE TABLE pgbench_accounts ("
							 "aid bigint not null primary key, bid bigint, abalance bigint, filler varchar(84))");
			executeStatement(con,
							 "CREATE TABLE pgbench_history ("
							 "tid bigint, bid bigint, aid bigint, delta bigint, mtime timestamptz, filler varchar(22), "
							 "PRIMARY KEY (aid, tid, bid))");
		}
		return;
	}

	initPQExpBuffer(&query);

	for (size_t i = 0; i < lengthof(DDLs); i++)
	{
		const struct ddlinfo *ddl = &DDLs[i];

		/* Construct new create table statement. */
		printfPQExpBuffer(&query, "create%s table %s(%s)",
						  (unlogged_tables && partition_method == PART_NONE &&
						   active_driver == &postgres_driver) ? " unlogged" : "",
						  ddl->table,
						  (scale >= SCALE_32BIT_THRESHOLD) ? ddl->bigcols : ddl->smcols);

		/* Partition pgbench_accounts table */
		if (partition_method != PART_NONE && strcmp(ddl->table, "pgbench_accounts") == 0)
			appendPQExpBuffer(&query,
							  " partition by %s (aid)", PARTITION_METHOD[partition_method]);
		else if (ddl->declare_fillfactor && active_driver == &postgres_driver)
		{
			/* fillfactor is only expected on actual tables */
			appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);
		}

		if (tablespace != NULL && active_driver == &postgres_driver)
		{
			char	   *escape_tablespace;

			escape_tablespace = active_driver->escape_identifier(con, tablespace, strlen(tablespace));
			appendPQExpBuffer(&query, " tablespace %s", escape_tablespace);
			active_driver->free_mem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);

	if (partition_method != PART_NONE)
		createPartitions(con);
}

/*
 * Truncate away any old data, in one command in case there are foreign keys
 */
static void
initTruncateTables(PgBenchConn *con)
{
	if (active_driver == &postgres_driver)
	{
		executeStatement(con, "truncate table "
						 "pgbench_accounts, "
						 "pgbench_branches, "
						 "pgbench_history, "
						 "pgbench_tellers");
	}
	else
	{
		executeStatement(con, "delete from pgbench_history where 1=1");
		executeStatement(con, "delete from pgbench_accounts where 1=1");
		executeStatement(con, "delete from pgbench_tellers where 1=1");
		executeStatement(con, "delete from pgbench_branches where 1=1");
	}
}

static void
initBranch(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t0\t\\N\n",
					  curr + 1);
}

static void
initTeller(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\\N\n",
					  curr + 1, curr / ntellers + 1);
}

static void
initAccount(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column defaults to blank padded empty string */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\n",
					  curr + 1, curr / naccounts + 1);
}

static void
initPopulateTable(PgBenchConn *con, const char *table, int64 base,
				  initRowMethod init_row)
{
	int			n;
	int64		k;
	int			chars = 0;
	int			prev_chars = 0;
	PgBenchCopyStream *stream;
	PQExpBufferData sql;
	char		copy_statement[256];
	const char *copy_statement_fmt = "copy %s from stdin";
	int64		total = base * scale;

	/* used to track elapsed time and estimate of the remaining time */
	pg_time_usec_t start;
	int			log_interval = 1;

	/* Stay on the same line if reporting to a terminal */
	char		eol = isatty(fileno(stderr)) ? '\r' : '\n';

	initPQExpBuffer(&sql);

	/* Use COPY with FREEZE on v14 and later for all ordinary tables */
	if (active_driver == &postgres_driver &&
		(PQserverVersion((PGconn *) con) >= 140000) &&
		get_table_relkind((PGconn *) con, table) == RELKIND_RELATION)
		copy_statement_fmt = "copy %s from stdin with (freeze on)";

	n = pg_snprintf(copy_statement, sizeof(copy_statement), copy_statement_fmt, table);
	if (n >= sizeof(copy_statement))
		pg_fatal("invalid buffer size: must be at least %d characters long", n);
	else if (n == -1)
		pg_fatal("invalid format string");

	stream = active_driver->copy_begin(con, table, 0, NULL, copy_statement);
	if (stream == NULL)
		pg_fatal("unexpected copy in result: %s", active_driver->error_message(con));

	start = pg_time_now();

	for (k = 0; k < total; k++)
	{
		int64		j = k + 1;

		init_row(&sql, k);
		if (!active_driver->copy_put_data(stream, sql.data, (int) sql.len))
			pg_fatal("copy_put_data failed: %s", active_driver->error_message(con));

		if (CancelRequested)
			break;

		/*
		 * If we want to stick with the original logging, print a message each
		 * 100k inserted rows.
		 */
		if ((!use_quiet) && (j % 100000 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
							j, total,
							(int) ((j * 100) / total),
							table, elapsed_sec, remaining_sec);

			/*
			 * If the previous progress message is longer than the current
			 * one, add spaces to the current line to fully overwrite any
			 * remaining characters from the previous message.
			 */
			if (prev_chars > chars)
				fprintf(stderr, "%*c", prev_chars - chars, ' ');
			fputc(eol, stderr);
			prev_chars = chars;
		}
		/* let's not call the timing for each row, but only each 100 rows */
		else if (use_quiet && (j % 100 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			/* have we reached the next interval (or end)? */
			if ((j == total) || (elapsed_sec >= log_interval * LOG_STEP_SECONDS))
			{
				chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
								j, total,
								(int) ((j * 100) / total),
								table, elapsed_sec, remaining_sec);

				/*
				 * If the previous progress message is longer than the current
				 * one, add spaces to the current line to fully overwrite any
				 * remaining characters from the previous message.
				 */
				if (prev_chars > chars)
					fprintf(stderr, "%*c", prev_chars - chars, ' ');
				fputc(eol, stderr);
				prev_chars = chars;

				/* skip to the next interval */
				log_interval = (int) ceil(elapsed_sec / LOG_STEP_SECONDS);
			}
		}
	}

	if (chars != 0 && eol != '\n')
		fprintf(stderr, "%*c\r", chars, ' ');	/* Clear the current line */

	if (!active_driver->copy_end(stream, NULL))
		pg_fatal("copy_end failed: %s", active_driver->error_message(con));

	termPQExpBuffer(&sql);
}

/*
 * Fill the standard tables with some data generated and sent from the client.
 *
 * The filler column is NULL in pgbench_branches and pgbench_tellers, and is
 * a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataClientSide(PgBenchConn *con)
{
	fprintf(stderr, "generating data (client-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	if (active_driver != &spanner_driver)
		executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	/*
	 * fill branches, tellers, accounts in that order in case foreign keys
	 * already exist
	 */
	initPopulateTable(con, "pgbench_branches", nbranches, initBranch);
	initPopulateTable(con, "pgbench_tellers", ntellers, initTeller);
	initPopulateTable(con, "pgbench_accounts", naccounts, initAccount);

	if (active_driver != &spanner_driver)
		executeStatement(con, "commit");
}

/*
 * Fill the standard tables with some data generated on the server
 *
 * As already the case with the client-side data generation, the filler
 * column defaults to NULL in pgbench_branches and pgbench_tellers,
 * and is a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataServerSide(PgBenchConn *con)
{
	PQExpBufferData sql;

	fprintf(stderr, "generating data (server-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_branches(bid,bbalance) "
					  "select bid, 0 "
					  "from generate_series(1, %d) as bid", nbranches * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_tellers(tid,bid,tbalance) "
					  "select tid, (tid - 1) / %d + 1, 0 "
					  "from generate_series(1, %d) as tid", ntellers, ntellers * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_accounts(aid,bid,abalance,filler) "
					  "select aid, (aid - 1) / %d + 1, 0, '' "
					  "from generate_series(1, " INT64_FORMAT ") as aid",
					  naccounts, (int64) naccounts * scale);
	executeStatement(con, sql.data);

	termPQExpBuffer(&sql);

	executeStatement(con, "commit");
}

/*
 * Invoke vacuum on the standard tables
 */
static void
initVacuum(PgBenchConn *con)
{
	fprintf(stderr, "vacuuming...\n");
	if (!active_driver->caps.supports_vacuum)
		return;
	executeStatement(con, "vacuum analyze pgbench_branches");
	executeStatement(con, "vacuum analyze pgbench_tellers");
	executeStatement(con, "vacuum analyze pgbench_accounts");
	executeStatement(con, "vacuum analyze pgbench_history");
}

/*
 * Create primary keys on the standard tables
 */
static void
initCreatePKeys(PgBenchConn *con)
{
	static const char *const DDLINDEXes[] = {
		"alter table pgbench_branches add primary key (bid)",
		"alter table pgbench_tellers add primary key (tid)",
		"alter table pgbench_accounts add primary key (aid)"
	};
	PQExpBufferData query;

	fprintf(stderr, "creating primary keys...\n");
	if (active_driver == &spanner_driver)
		return;

	initPQExpBuffer(&query);

	for (size_t i = 0; i < lengthof(DDLINDEXes); i++)
	{
		resetPQExpBuffer(&query);
		appendPQExpBufferStr(&query, DDLINDEXes[i]);

		if (index_tablespace != NULL && active_driver == &postgres_driver)
		{
			char	   *escape_tablespace;

			escape_tablespace = active_driver->escape_identifier(con, index_tablespace,
																 strlen(index_tablespace));
			appendPQExpBuffer(&query, " using index tablespace %s", escape_tablespace);
			active_driver->free_mem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create foreign key constraints between the standard tables
 */
static void
initCreateFKeys(PgBenchConn *con)
{
	static const char *const DDLKEYs[] = {
		"alter table pgbench_tellers add constraint pgbench_tellers_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_accounts add constraint pgbench_accounts_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_tid_fkey foreign key (tid) references pgbench_tellers",
		"alter table pgbench_history add constraint pgbench_history_aid_fkey foreign key (aid) references pgbench_accounts"
	};

	fprintf(stderr, "creating foreign keys...\n");
	for (size_t i = 0; i < lengthof(DDLKEYs); i++)
	{
		executeStatement(con, DDLKEYs[i]);
	}
}

/*
 * Validate an initialization-steps string
 *
 * (We could just leave it to runInitSteps() to fail if there are wrong
 * characters, but since initialization can take awhile, it seems friendlier
 * to check during option parsing.)
 */
void
checkInitSteps(const char *initialize_steps)
{
	if (initialize_steps[0] == '\0')
		pg_fatal("no initialization steps specified");

	for (const char *step = initialize_steps; *step != '\0'; step++)
	{
		if (strchr(ALL_INIT_STEPS " ", *step) == NULL)
		{
			pg_log_error("unrecognized initialization step \"%c\"", *step);
			pg_log_error_detail("Allowed step characters are: \"" ALL_INIT_STEPS "\".");
			exit(1);
		}
	}
}

/*
 * Invoke each initialization step in the given string
 */
void
runInitSteps(const char *initialize_steps)
{
	PQExpBufferData stats;
	PgBenchConn *con;
	const char *step;
	double		run_time = 0.0;
	bool		first = true;

	initPQExpBuffer(&stats);

	if ((con = doConnect()) == NULL)
		pg_fatal("could not create connection for initialization");

	setup_cancel_handler(NULL);
	active_driver->set_cancel_conn(con);

	for (step = initialize_steps; *step != '\0'; step++)
	{
		char	   *op = NULL;
		pg_time_usec_t start = pg_time_now();

		switch (*step)
		{
			case 'd':
				op = "drop tables";
				initDropTables(con);
				break;
			case 't':
				op = "create tables";
				initCreateTables(con);
				break;
			case 'g':
				op = "client-side generate";
				initGenerateDataClientSide(con);
				break;
			case 'G':
				op = "server-side generate";
				initGenerateDataServerSide(con);
				break;
			case 'v':
				op = "vacuum";
				initVacuum(con);
				break;
			case 'p':
				op = "primary keys";
				initCreatePKeys(con);
				break;
			case 'f':
				op = "foreign keys";
				initCreateFKeys(con);
				break;
			case ' ':
				break;			/* ignore */
			default:
				pg_log_error("unrecognized initialization step \"%c\"", *step);
				active_driver->finish(con);
				exit(1);
		}

		if (op != NULL)
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);

			if (!first)
				appendPQExpBufferStr(&stats, ", ");
			else
				first = false;

			appendPQExpBuffer(&stats, "%s %.2f s", op, elapsed_sec);

			run_time += elapsed_sec;
		}
	}

	fprintf(stderr, "done in %.2f s (%s).\n", run_time, stats.data);
	active_driver->reset_cancel_conn();
	active_driver->finish(con);
	termPQExpBuffer(&stats);
}

/*
 * Extract pgbench table information into global variables scale,
 * partition_method and partitions.
 */
void
GetTableInfo(PgBenchConn *con, bool scale_given)
{
	PgBenchResult *res;

	/*
	 * get the scaling factor that should be same as count(*) from
	 * pgbench_branches if this is not a custom query
	 */
	res = active_driver->exec_sync(con, "select count(*) from pgbench_branches");
	if (active_driver->result_status(res) != PGBENCH_RES_TUPLES_OK)
	{
		const char *sqlState = active_driver->result_error_field(res, PG_DIAG_SQLSTATE);

		pg_log_error("could not count number of branches: %s", active_driver->error_message(con));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) == 0)
			pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".",
							  active_driver->get_db(con));

		exit(1);
	}
	scale = atoi(active_driver->get_value(res, 0, 0));
	if (scale < 0)
		pg_fatal("invalid count(*) from pgbench_branches: \"%s\"",
				 active_driver->get_value(res, 0, 0));
	active_driver->clear_result(res);

	/* warn if we override user-given -s switch */
	if (scale_given)
		pg_log_warning("scale option ignored, using count from pgbench_branches table (%d)",
					   scale);

	if (!active_driver->caps.supports_pg_catalog)
	{
		partition_method = PART_NONE;
		partitions = 0;
		return;
	}

	/*
	 * Get the partition information for the first "pgbench_accounts" table
	 * found in search_path.
	 *
	 * The result is empty if no "pgbench_accounts" is found.
	 *
	 * Otherwise, it always returns one row even if the table is not
	 * partitioned (in which case the partition strategy is NULL).
	 *
	 * The number of partitions can be 0 even for partitioned tables, if no
	 * partition is attached.
	 *
	 * We assume no partitioning on any failure, so as to avoid failing on an
	 * old version without "pg_partitioned_table".
	 */
	res = active_driver->exec_sync(con,
								   "select o.n, p.partstrat, pg_catalog.count(i.inhparent) "
								   "from pg_catalog.pg_class as c "
								   "join pg_catalog.pg_namespace as n on (n.oid = c.relnamespace) "
								   "cross join lateral (select pg_catalog.array_position(pg_catalog.current_schemas(true), n.nspname)) as o(n) "
								   "left join pg_catalog.pg_partitioned_table as p on (p.partrelid = c.oid) "
								   "left join pg_catalog.pg_inherits as i on (c.oid = i.inhparent) "
								   "where c.relname = 'pgbench_accounts' and o.n is not null "
								   "group by 1, 2 "
								   "order by 1 asc "
								   "limit 1");

	if (active_driver->result_status(res) != PGBENCH_RES_TUPLES_OK)
	{
		/* probably an older version, coldly assume no partitioning */
		partition_method = PART_NONE;
		partitions = 0;
	}
	else if (active_driver->ntuples(res) == 0)
	{
		/*
		 * This case is unlikely as pgbench already found "pgbench_branches"
		 * above to compute the scale.
		 */
		pg_log_error("no pgbench_accounts table found in \"search_path\"");
		pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".",
						  active_driver->get_db(con));
		exit(1);
	}
	else						/* active_driver->ntuples(res) == 1 */
	{
		/* normal case, extract partition information */
		if (active_driver->get_is_null(res, 0, 1))
			partition_method = PART_NONE;
		else
		{
			const char *ps = active_driver->get_value(res, 0, 1);

			/* column must be there */
			Assert(ps != NULL);

			if (strcmp(ps, "r") == 0)
				partition_method = PART_RANGE;
			else if (strcmp(ps, "h") == 0)
				partition_method = PART_HASH;
			else
			{
				/* possibly a newer version with new partition method */
				pg_fatal("unexpected partition method: \"%s\"", ps);
			}
		}

		partitions = atoi(active_driver->get_value(res, 0, 2));
	}

	active_driver->clear_result(res);
}
