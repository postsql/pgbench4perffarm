/*-------------------------------------------------------------------------
 *
 * neo4j_driver.c
 *		Neo4j Cypher HTTP Transaction API database driver implementation of
 *		PgBenchDbDriver for pgbench.
 *
 * Implements the Neo4j HTTP Transaction Cypher API (/db/<database>/tx,
 * /db/<database>/tx/<id>, /db/<database>/tx/<id>/commit, and
 * /db/<database>/tx/commit) over non-blocking TCP sockets (with optional
 * OpenSSL TLS for neo4j+s://, bolt+s://, https://):
 *   - Connection URI parsing: neo4j://, neo4j+s://, bolt://, bolt+s://
 *     with HTTP Basic or Bearer token authentication (default port 7474,
 *     default database "neo4j").
 *   - Parameter translation from PostgreSQL $1, $2 placeholders to Cypher
 *     $p1, $p2 parameters with typed JSON "parameters": {"p1": ...} maps.
 *   - Explicit multi-statement transaction lifecycle: BEGIN opens a server
 *     transaction via POST /db/<db>/tx, queries execute against
 *     POST /db/<db>/tx/<id>, COMMIT commits via POST /db/<db>/tx/<id>/commit,
 *     and ROLLBACK aborts via DELETE /db/<db>/tx/<id>.
 *   - Pipeline mode (\startpipeline / \endpipeline) batching multiple Cypher
 *     statements into a single HTTP request {"statements": [...]} and
 *     unpacking "results": [...] into individual PgBenchResult objects.
 *   - Automatic mapping of Neo.TransientError.* (deadlocks, lock timeouts,
 *     outdated transactions) to SQLSTATE "40001" for pgbench retries.
 *   - Bulk dataset ingestion via batched UNWIND $rows AS row CREATE (n:Label)
 *     SET n = row statements.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/neo4j_driver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef USE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include "common/logging.h"
#include "common/string.h"
#include "db_driver.h"
#include "fe_utils/string_utils.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "variable.h"

#define NEO4J_DEFAULT_PORT "7474"
#define NEO4J_DEFAULT_DB   "neo4j"

/*-------------------------------------------------------------------------
 * Minimal JSON parser for Neo4j HTTP Transaction API responses
 *-------------------------------------------------------------------------
 */

typedef enum Neo4jJsonType
{
	NJSON_NULL = 0,
	NJSON_BOOL,
	NJSON_NUMBER,
	NJSON_STRING,
	NJSON_ARRAY,
	NJSON_OBJECT
} Neo4jJsonType;

typedef struct Neo4jJsonNode Neo4jJsonNode;

typedef struct Neo4jJsonField
{
	char	   *key;
	Neo4jJsonNode *val;
} Neo4jJsonField;

struct Neo4jJsonNode
{
	Neo4jJsonType type;
	bool		bval;
	char	   *str;
	Neo4jJsonNode **elems;
	int			nelems;
	int			elem_cap;
	Neo4jJsonField *fields;
	int			nfields;
	int			field_cap;
};

static void
neo4j_json_free(Neo4jJsonNode *node)
{
	if (node == NULL)
		return;
	pg_free(node->str);
	if (node->elems)
	{
		for (int i = 0; i < node->nelems; i++)
			neo4j_json_free(node->elems[i]);
		pg_free(node->elems);
	}
	if (node->fields)
	{
		for (int i = 0; i < node->nfields; i++)
		{
			pg_free(node->fields[i].key);
			neo4j_json_free(node->fields[i].val);
		}
		pg_free(node->fields);
	}
	pg_free(node);
}

static void
neo4j_json_skip_ws(const char **pp)
{
	const char *p = *pp;

	while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
		p++;
	*pp = p;
}

static char *
neo4j_json_parse_str(const char **pp)
{
	const char *p = *pp;
	PQExpBufferData buf;

	if (*p != '"')
		return NULL;
	p++;
	initPQExpBuffer(&buf);
	while (*p && *p != '"')
	{
		if (*p == '\\' && p[1] != '\0')
		{
			p++;
			switch (*p)
			{
				case '"':
				case '\\':
				case '/':
					appendPQExpBufferChar(&buf, *p++);
					break;
				case 'b':
					appendPQExpBufferChar(&buf, '\b');
					p++;
					break;
				case 'f':
					appendPQExpBufferChar(&buf, '\f');
					p++;
					break;
				case 'n':
					appendPQExpBufferChar(&buf, '\n');
					p++;
					break;
				case 'r':
					appendPQExpBufferChar(&buf, '\r');
					p++;
					break;
				case 't':
					appendPQExpBufferChar(&buf, '\t');
					p++;
					break;
				case 'u':
					if (isxdigit((unsigned char) p[1]) &&
						isxdigit((unsigned char) p[2]) &&
						isxdigit((unsigned char) p[3]) &&
						isxdigit((unsigned char) p[4]))
					{
						char		hex[5];
						unsigned int cp;

						memcpy(hex, p + 1, 4);
						hex[4] = '\0';
						cp = (unsigned int) strtoul(hex, NULL, 16);
						if (cp < 0x80)
							appendPQExpBufferChar(&buf, (char) cp);
						else if (cp < 0x800)
						{
							appendPQExpBufferChar(&buf, (char) (0xC0 | (cp >> 6)));
							appendPQExpBufferChar(&buf, (char) (0x80 | (cp & 0x3F)));
						}
						else
						{
							appendPQExpBufferChar(&buf, (char) (0xE0 | (cp >> 12)));
							appendPQExpBufferChar(&buf, (char) (0x80 | ((cp >> 6) & 0x3F)));
							appendPQExpBufferChar(&buf, (char) (0x80 | (cp & 0x3F)));
						}
						p += 5;
					}
					else
					{
						appendPQExpBufferChar(&buf, *p++);
					}
					break;
				default:
					appendPQExpBufferChar(&buf, *p++);
					break;
			}
		}
		else
		{
			appendPQExpBufferChar(&buf, *p++);
		}
	}
	if (*p == '"')
		p++;
	*pp = p;
	return buf.data;
}

static Neo4jJsonNode *
neo4j_json_parse_value(const char **pp)
{
	const char *p;
	Neo4jJsonNode *node;

	neo4j_json_skip_ws(pp);
	p = *pp;
	if (*p == '\0')
		return NULL;

	if (*p == '"')
	{
		char	   *s = neo4j_json_parse_str(pp);

		if (s == NULL)
			return NULL;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_STRING;
		node->str = s;
		return node;
	}

	if (*p == '{')
	{
		p++;
		*pp = p;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_OBJECT;
		neo4j_json_skip_ws(pp);
		if (**pp == '}')
		{
			(*pp)++;
			return node;
		}
		for (;;)
		{
			char	   *key;
			Neo4jJsonNode *val;

			neo4j_json_skip_ws(pp);
			key = neo4j_json_parse_str(pp);
			if (key == NULL)
			{
				neo4j_json_free(node);
				return NULL;
			}
			neo4j_json_skip_ws(pp);
			if (**pp != ':')
			{
				pg_free(key);
				neo4j_json_free(node);
				return NULL;
			}
			(*pp)++;
			val = neo4j_json_parse_value(pp);
			if (val == NULL)
			{
				pg_free(key);
				neo4j_json_free(node);
				return NULL;
			}
			if (node->nfields >= node->field_cap)
			{
				node->field_cap = node->field_cap ? node->field_cap * 2 : 8;
				node->fields = pg_realloc_array(node->fields, Neo4jJsonField, node->field_cap);
			}
			node->fields[node->nfields].key = key;
			node->fields[node->nfields].val = val;
			node->nfields++;

			neo4j_json_skip_ws(pp);
			if (**pp == ',')
			{
				(*pp)++;
				continue;
			}
			if (**pp == '}')
			{
				(*pp)++;
				break;
			}
			neo4j_json_free(node);
			return NULL;
		}
		return node;
	}

	if (*p == '[')
	{
		p++;
		*pp = p;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_ARRAY;
		neo4j_json_skip_ws(pp);
		if (**pp == ']')
		{
			(*pp)++;
			return node;
		}
		for (;;)
		{
			Neo4jJsonNode *elem = neo4j_json_parse_value(pp);

			if (elem == NULL)
			{
				neo4j_json_free(node);
				return NULL;
			}
			if (node->nelems >= node->elem_cap)
			{
				node->elem_cap = node->elem_cap ? node->elem_cap * 2 : 8;
				node->elems = pg_realloc_array(node->elems, Neo4jJsonNode *, node->elem_cap);
			}
			node->elems[node->nelems++] = elem;

			neo4j_json_skip_ws(pp);
			if (**pp == ',')
			{
				(*pp)++;
				continue;
			}
			if (**pp == ']')
			{
				(*pp)++;
				break;
			}
			neo4j_json_free(node);
			return NULL;
		}
		return node;
	}

	if (strncmp(p, "true", 4) == 0)
	{
		*pp = p + 4;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_BOOL;
		node->bval = true;
		node->str = pg_strdup("true");
		return node;
	}

	if (strncmp(p, "false", 5) == 0)
	{
		*pp = p + 5;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_BOOL;
		node->bval = false;
		node->str = pg_strdup("false");
		return node;
	}

	if (strncmp(p, "null", 4) == 0)
	{
		*pp = p + 4;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_NULL;
		return node;
	}

	if (*p == '-' || isdigit((unsigned char) *p))
	{
		const char *start = p;

		if (*p == '-')
			p++;
		while (isdigit((unsigned char) *p) || *p == '.' || *p == 'e' || *p == 'E' ||
			   *p == '+' || *p == '-')
			p++;
		*pp = p;
		node = pg_malloc0_object(Neo4jJsonNode);
		node->type = NJSON_NUMBER;
		node->str = pnstrdup(start, p - start);
		return node;
	}

	return NULL;
}

static Neo4jJsonNode *
neo4j_json_get_field(const Neo4jJsonNode *obj, const char *key)
{
	if (obj == NULL || obj->type != NJSON_OBJECT)
		return NULL;
	for (int i = 0; i < obj->nfields; i++)
	{
		if (strcmp(obj->fields[i].key, key) == 0)
			return obj->fields[i].val;
	}
	return NULL;
}

static void
neo4j_append_json_str(PQExpBuffer buf, const char *str)
{
	if (str == NULL)
	{
		appendPQExpBufferStr(buf, "null");
		return;
	}
	appendPQExpBufferChar(buf, '"');
	for (const char *p = str; *p; p++)
	{
		unsigned char c = (unsigned char) *p;

		switch (c)
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
				if (c < 0x20)
					appendPQExpBuffer(buf, "\\u%04x", (unsigned int) c);
				else
					appendPQExpBufferChar(buf, (char) c);
				break;
		}
	}
	appendPQExpBufferChar(buf, '"');
}

static void
neo4j_serialize_json_node(PQExpBuffer buf, const Neo4jJsonNode *node)
{
	if (node == NULL || node->type == NJSON_NULL)
	{
		appendPQExpBufferStr(buf, "null");
		return;
	}
	switch (node->type)
	{
		case NJSON_BOOL:
			appendPQExpBufferStr(buf, node->bval ? "true" : "false");
			break;
		case NJSON_NUMBER:
			appendPQExpBufferStr(buf, node->str ? node->str : "0");
			break;
		case NJSON_STRING:
			neo4j_append_json_str(buf, node->str ? node->str : "");
			break;
		case NJSON_ARRAY:
			appendPQExpBufferChar(buf, '[');
			for (int i = 0; i < node->nelems; i++)
			{
				if (i > 0)
					appendPQExpBufferChar(buf, ',');
				neo4j_serialize_json_node(buf, node->elems[i]);
			}
			appendPQExpBufferChar(buf, ']');
			break;
		case NJSON_OBJECT:
			appendPQExpBufferChar(buf, '{');
			for (int i = 0; i < node->nfields; i++)
			{
				if (i > 0)
					appendPQExpBufferChar(buf, ',');
				neo4j_append_json_str(buf, node->fields[i].key);
				appendPQExpBufferChar(buf, ':');
				neo4j_serialize_json_node(buf, node->fields[i].val);
			}
			appendPQExpBufferChar(buf, '}');
			break;
		default:
			appendPQExpBufferStr(buf, "null");
			break;
	}
}

/*-------------------------------------------------------------------------
 * Base64 encoder for HTTP Basic Auth
 *-------------------------------------------------------------------------
 */

static char *
neo4j_base64_encode(const uint8 *src, size_t len)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t		out_len = 4 * ((len + 2) / 3);
	char	   *out = pg_malloc(out_len + 1);
	size_t		i = 0;
	size_t		j = 0;

	while (i < len)
	{
		uint32		a = i < len ? src[i++] : 0;
		uint32		b = i < len ? src[i++] : 0;
		uint32		c = i < len ? src[i++] : 0;
		uint32		triple = (a << 16) | (b << 8) | c;

		out[j++] = b64[(triple >> 18) & 0x3F];
		out[j++] = b64[(triple >> 12) & 0x3F];
		out[j++] = b64[(triple >> 6) & 0x3F];
		out[j++] = b64[triple & 0x3F];
	}

	if (len % 3 == 1)
	{
		out[out_len - 1] = '=';
		out[out_len - 2] = '=';
	}
	else if (len % 3 == 2)
	{
		out[out_len - 1] = '=';
	}
	out[out_len] = '\0';
	return out;
}

/*-------------------------------------------------------------------------
 * Driver Structures
 *-------------------------------------------------------------------------
 */

typedef enum Neo4jReqKind
{
	NEO4J_REQ_AUTOCOMMIT = 0,
	NEO4J_REQ_BEGIN,
	NEO4J_REQ_IN_TX,
	NEO4J_REQ_COMMIT,
	NEO4J_REQ_ROLLBACK,
	NEO4J_REQ_PIPELINE_BATCH
} Neo4jReqKind;

typedef struct Neo4jPreparedStmt
{
	char	   *name;
	char	   *cypher;
} Neo4jPreparedStmt;

typedef struct Neo4jPendingHttp
{
	Neo4jReqKind kind;
	int			expected_stmts;
	bool		has_pipeline_sync;
	struct Neo4jPendingHttp *next;
} Neo4jPendingHttp;

struct PgBenchResult
{
	PgBenchExecStatus status;
	char	   *err_msg;
	char		sqlstate[6];
	int			nrows;
	int			ncols;
	char	  **col_names;
	char	 ***cells;
	bool	  **is_null;
	struct PgBenchResult *next;
};

struct PgBenchCopyStream
{
	PgBenchConn *conn;
	char	   *table_name;
	char	  **col_names;
	int			ncols;
	PQExpBufferData line_buf;
	PQExpBufferData rows_json;
	int			buffered_rows;
	bool		failed;
};

struct PgBenchConn
{
	int			sock;
	bool		use_tls;
#ifdef USE_OPENSSL
	SSL_CTX    *ssl_ctx;
	SSL		   *ssl;
	bool		ssl_handshake_done;
#endif
	PgBenchConnStatus status;
	PgBenchTxStatus tx_status;
	PgBenchPipelineStatus pipe_status;
	bool		pipeline_aborted;

	char	   *host;
	char	   *port;
	char	   *dbname;
	char	   *user;
	char	   *password;
	char	   *auth_header;	/* e.g. "Basic ..." or "Bearer ..." */
	PQExpBufferData err_buf;

	/* Active transaction endpoint path (e.g. "/db/neo4j/tx/12") */
	char	   *active_tx_path;

	/* Non-blocking write buffer */
	char	   *out_buf;
	size_t		out_len;
	size_t		out_sent;
	size_t		out_cap;

	/* Non-blocking read buffer */
	char	   *in_buf;
	size_t		in_len;
	size_t		in_cap;

	/* Pending HTTP request FIFO */
	Neo4jPendingHttp *pending_head;
	Neo4jPendingHttp *pending_tail;

	/* Pipeline buffered statement JSON objects */
	PQExpBufferData pipe_stmts_buf;
	int			pipe_stmts_count;

	/* Completed results FIFO */
	PgBenchResult *res_head;
	PgBenchResult *res_tail;
	bool		result_drained;

	/* Prepared statement registry */
	Neo4jPreparedStmt *prep_stmts;
	int			nprep_stmts;
	int			prep_cap;
};

static char *saved_neo4j_conninfo = NULL;

/*-------------------------------------------------------------------------
 * Result & Queue Helpers
 *-------------------------------------------------------------------------
 */

static PgBenchResult *
neo4j_alloc_result(PgBenchExecStatus status)
{
	PgBenchResult *res = pg_malloc0_object(PgBenchResult);

	res->status = status;
	return res;
}

static void
neo4j_free_result(PgBenchResult *res)
{
	if (res == NULL)
		return;
	pg_free(res->err_msg);
	if (res->col_names)
	{
		for (int c = 0; c < res->ncols; c++)
			pg_free(res->col_names[c]);
		pg_free(res->col_names);
	}
	if (res->cells)
	{
		for (int r = 0; r < res->nrows; r++)
		{
			if (res->cells[r])
			{
				for (int c = 0; c < res->ncols; c++)
					pg_free(res->cells[r][c]);
				pg_free(res->cells[r]);
			}
		}
		pg_free(res->cells);
	}
	if (res->is_null)
	{
		for (int r = 0; r < res->nrows; r++)
			pg_free(res->is_null[r]);
		pg_free(res->is_null);
	}
	pg_free(res);
}

static void
neo4j_push_result(PgBenchConn *conn, PgBenchResult *res)
{
	res->next = NULL;
	if (conn->res_tail)
		conn->res_tail->next = res;
	else
		conn->res_head = res;
	conn->res_tail = res;
	conn->result_drained = false;
}

static void
neo4j_push_pending(PgBenchConn *conn, Neo4jReqKind kind,
				   int expected_stmts, bool has_pipeline_sync)
{
	Neo4jPendingHttp *req = pg_malloc0_object(Neo4jPendingHttp);

	req->kind = kind;
	req->expected_stmts = expected_stmts;
	req->has_pipeline_sync = has_pipeline_sync;
	if (conn->pending_tail)
		conn->pending_tail->next = req;
	else
		conn->pending_head = req;
	conn->pending_tail = req;
	conn->result_drained = false;
}

static void
neo4j_pop_pending(PgBenchConn *conn)
{
	Neo4jPendingHttp *req = conn->pending_head;

	if (req == NULL)
		return;
	conn->pending_head = req->next;
	if (conn->pending_head == NULL)
		conn->pending_tail = NULL;
	pg_free(req);
}

/*-------------------------------------------------------------------------
 * URI & Connection Parameter Parsing
 *-------------------------------------------------------------------------
 */

static void
neo4j_parse_uri(const char *uri, bool *use_tls_out,
				char **host_out, char **port_out,
				char **user_out, char **pass_out, char **db_out)
{
	const char *p = NULL;
	const char *at;
	const char *slash;
	const char *qmark;
	const char *host_start;
	const char *host_end;

	if (pg_strncasecmp(uri, "neo4j+s://", 10) == 0 ||
		pg_strncasecmp(uri, "neo4j+ssc://", 12) == 0)
	{
		*use_tls_out = true;
		p = strchr(uri, '/') + 2;
	}
	else if (pg_strncasecmp(uri, "bolt+s://", 9) == 0 ||
			 pg_strncasecmp(uri, "bolt+ssc://", 11) == 0)
	{
		*use_tls_out = true;
		p = strchr(uri, '/') + 2;
	}
	else if (pg_strncasecmp(uri, "https://", 8) == 0)
	{
		*use_tls_out = true;
		p = uri + 8;
	}
	else if (pg_strncasecmp(uri, "neo4j://", 8) == 0)
	{
		*use_tls_out = false;
		p = uri + 8;
	}
	else if (pg_strncasecmp(uri, "bolt://", 7) == 0)
	{
		*use_tls_out = false;
		p = uri + 7;
	}
	else if (pg_strncasecmp(uri, "http://", 7) == 0)
	{
		*use_tls_out = false;
		p = uri + 7;
	}
	else
	{
		return;
	}

	qmark = strchr(p, '?');
	slash = strchr(p, '/');
	if (slash && qmark && slash > qmark)
		slash = NULL;

	at = strchr(p, '@');
	if (at && ((slash && at > slash) || (qmark && at > qmark)))
		at = NULL;

	if (at != NULL)
	{
		const char *colon = memchr(p, ':', at - p);

		if (colon != NULL)
		{
			if (colon > p)
			{
				pg_free(*user_out);
				*user_out = pnstrdup(p, colon - p);
			}
			pg_free(*pass_out);
			*pass_out = pnstrdup(colon + 1, at - (colon + 1));
		}
		else if (at > p)
		{
			pg_free(*user_out);
			*user_out = pnstrdup(p, at - p);
		}
		host_start = at + 1;
	}
	else
	{
		host_start = p;
	}

	host_end = slash ? slash : (qmark ? qmark : (p + strlen(p)));
	if (host_start < host_end)
	{
		if (*host_start == '[')
		{
			const char *rb = memchr(host_start, ']', host_end - host_start);

			if (rb != NULL)
			{
				pg_free(*host_out);
				*host_out = pnstrdup(host_start + 1, rb - (host_start + 1));
				if (rb + 1 < host_end && rb[1] == ':')
				{
					pg_free(*port_out);
					*port_out = pnstrdup(rb + 2, host_end - (rb + 2));
				}
			}
		}
		else
		{
			const char *colon = memchr(host_start, ':', host_end - host_start);

			if (colon != NULL)
			{
				if (colon > host_start)
				{
					pg_free(*host_out);
					*host_out = pnstrdup(host_start, colon - host_start);
				}
				if (colon + 1 < host_end)
				{
					pg_free(*port_out);
					*port_out = pnstrdup(colon + 1, host_end - (colon + 1));
				}
			}
			else
			{
				pg_free(*host_out);
				*host_out = pnstrdup(host_start, host_end - host_start);
			}
		}
	}

	if (slash != NULL)
	{
		const char *db_end = qmark ? qmark : (uri + strlen(uri));

		if (slash + 1 < db_end)
		{
			pg_free(*db_out);
			*db_out = pnstrdup(slash + 1, db_end - (slash + 1));
		}
	}
}

/*-------------------------------------------------------------------------
 * Socket Connect, Reconnect, and HTTP I/O
 *-------------------------------------------------------------------------
 */

static bool
neo4j_open_socket(PgBenchConn *conn, bool nonblocking_connect)
{
	struct addrinfo hints;
	struct addrinfo *addrs = NULL;
	struct addrinfo *rp;
	int			rc;

	if (conn->sock >= 0)
	{
#ifdef USE_OPENSSL
		if (conn->ssl)
		{
			SSL_free(conn->ssl);
			conn->ssl = NULL;
		}
#endif
		close(conn->sock);
		conn->sock = -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	rc = getaddrinfo(conn->host, conn->port, &hints, &addrs);
	if (rc != 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not resolve Neo4j host \"%s:%s\": %s\n",
						  conn->host, conn->port, gai_strerror(rc));
		conn->status = PGBENCH_CONN_BAD;
		return false;
	}

	for (rp = addrs; rp != NULL; rp = rp->ai_next)
	{
		int			s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		int			flags;
		int			one = 1;

		if (s < 0)
			continue;

		(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		flags = fcntl(s, F_GETFL, 0);
		if (nonblocking_connect && flags >= 0)
			(void) fcntl(s, F_SETFL, flags | O_NONBLOCK);

		if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0 ||
			(nonblocking_connect && errno == EINPROGRESS))
		{
			if (!nonblocking_connect && flags >= 0)
				(void) fcntl(s, F_SETFL, flags | O_NONBLOCK);
			conn->sock = s;
			break;
		}
		close(s);
	}

	freeaddrinfo(addrs);

	if (conn->sock < 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not connect to Neo4j at \"%s:%s\": %s\n",
						  conn->host, conn->port, strerror(errno));
		conn->status = PGBENCH_CONN_BAD;
		return false;
	}

#ifdef USE_OPENSSL
	if (conn->use_tls && !nonblocking_connect)
	{
		if (conn->ssl_ctx == NULL)
		{
			conn->ssl_ctx = SSL_CTX_new(TLS_client_method());
			if (conn->ssl_ctx)
				SSL_CTX_set_verify(conn->ssl_ctx, SSL_VERIFY_NONE, NULL);
		}
		conn->ssl = SSL_new(conn->ssl_ctx);
		SSL_set_fd(conn->ssl, conn->sock);
		SSL_set_tlsext_host_name(conn->ssl, conn->host);
		for (;;)
		{
			int			sret = SSL_connect(conn->ssl);

			if (sret == 1)
			{
				conn->ssl_handshake_done = true;
				break;
			}
			{
				int			serr = SSL_get_error(conn->ssl, sret);
				struct pollfd pfd;

				if (serr != SSL_ERROR_WANT_READ && serr != SSL_ERROR_WANT_WRITE)
				{
					printfPQExpBuffer(&conn->err_buf, "Neo4j TLS handshake failed\n");
					conn->status = PGBENCH_CONN_BAD;
					return false;
				}
				pfd.fd = conn->sock;
				pfd.events = (serr == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
				pfd.revents = 0;
				if (poll(&pfd, 1, 10000) <= 0)
				{
					printfPQExpBuffer(&conn->err_buf, "Neo4j TLS handshake timed out\n");
					conn->status = PGBENCH_CONN_BAD;
					return false;
				}
			}
		}
	}
#endif

	return true;
}

static bool
neo4j_ensure_connected(PgBenchConn *conn)
{
	char		probe;
	ssize_t		n;

	if (conn->sock < 0)
		return neo4j_open_socket(conn, false);

	if (conn->pending_head == NULL && conn->out_sent == conn->out_len)
	{
		n = recv(conn->sock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			return neo4j_open_socket(conn, false);
	}

	return true;
}

static void
neo4j_queue_bytes(PgBenchConn *conn, const char *data, size_t len)
{
	if (conn->out_sent == conn->out_len)
	{
		conn->out_sent = 0;
		conn->out_len = 0;
	}
	if (conn->out_len + len > conn->out_cap)
	{
		size_t		new_cap = conn->out_cap ? conn->out_cap * 2 : 8192;

		while (new_cap < conn->out_len + len)
			new_cap *= 2;
		conn->out_buf = pg_realloc(conn->out_buf, new_cap);
		conn->out_cap = new_cap;
	}
	memcpy(conn->out_buf + conn->out_len, data, len);
	conn->out_len += len;
}

static bool
neo4j_flush_out(PgBenchConn *conn)
{
	while (conn->out_sent < conn->out_len)
	{
		ssize_t		n;

#ifdef USE_OPENSSL
		if (conn->use_tls && conn->ssl)
		{
			n = SSL_write(conn->ssl,
						  conn->out_buf + conn->out_sent,
						  (int) (conn->out_len - conn->out_sent));
			if (n <= 0)
			{
				int			serr = SSL_get_error(conn->ssl, (int) n);

				if (serr == SSL_ERROR_WANT_READ || serr == SSL_ERROR_WANT_WRITE)
					return true;
				printfPQExpBuffer(&conn->err_buf, "Neo4j SSL_write failed\n");
				conn->status = PGBENCH_CONN_BAD;
				return false;
			}
			conn->out_sent += (size_t) n;
			continue;
		}
#endif
		n = send(conn->sock,
				 conn->out_buf + conn->out_sent,
				 conn->out_len - conn->out_sent,
#ifdef MSG_NOSIGNAL
				 MSG_NOSIGNAL
#else
				 0
#endif
			);
		if (n > 0)
		{
			conn->out_sent += (size_t) n;
		}
		else if (n < 0 && errno == EINTR)
		{
			continue;
		}
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			return true;
		}
		else
		{
			printfPQExpBuffer(&conn->err_buf,
							  "Neo4j HTTP send failed: %s\n", strerror(errno));
			conn->status = PGBENCH_CONN_BAD;
			return false;
		}
	}

	conn->out_sent = 0;
	conn->out_len = 0;
	return true;
}

static bool
neo4j_send_http_request(PgBenchConn *conn, const char *method,
						const char *path, const char *json_body,
						Neo4jReqKind kind, int expected_stmts,
						bool has_pipeline_sync)
{
	PQExpBufferData req;
	size_t		body_len = json_body ? strlen(json_body) : 0;

	if (!neo4j_ensure_connected(conn))
		return false;

	initPQExpBuffer(&req);
	appendPQExpBuffer(&req,
					  "%s %s HTTP/1.1\r\n"
					  "Host: %s:%s\r\n"
					  "Accept: application/json;charset=UTF-8\r\n"
					  "Content-Type: application/json\r\n"
					  "Connection: keep-alive\r\n",
					  method, path, conn->host, conn->port);
	if (conn->auth_header && conn->auth_header[0] != '\0')
		appendPQExpBuffer(&req, "Authorization: %s\r\n", conn->auth_header);
	appendPQExpBuffer(&req, "Content-Length: %zu\r\n\r\n", body_len);
	if (body_len > 0)
		appendBinaryPQExpBuffer(&req, json_body, body_len);

	neo4j_queue_bytes(conn, req.data, req.len);
	termPQExpBuffer(&req);

	neo4j_push_pending(conn, kind, expected_stmts, has_pipeline_sync);
	return neo4j_flush_out(conn);
}

/*-------------------------------------------------------------------------
 * Cypher Parameter Translation ($1 -> $p1) & SQL Compatibility
 *-------------------------------------------------------------------------
 */

typedef enum Neo4jSqlControlType
{
	NEO4J_CTRL_NONE = 0,
	NEO4J_CTRL_NOOP,
	NEO4J_CTRL_BEGIN,
	NEO4J_CTRL_COMMIT,
	NEO4J_CTRL_ROLLBACK
} Neo4jSqlControlType;

static char *
neo4j_translate_dollar_params(const char *sql)
{
	PQExpBufferData out;
	const char *p = sql;

	initPQExpBuffer(&out);
	while (*p)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			char		quote = *p;

			appendPQExpBufferChar(&out, *p++);
			while (*p)
			{
				if (*p == '\\' && p[1] != '\0')
				{
					appendPQExpBufferChar(&out, *p++);
					appendPQExpBufferChar(&out, *p++);
					continue;
				}
				if (*p == quote)
				{
					appendPQExpBufferChar(&out, *p++);
					if (*p == quote)
					{
						appendPQExpBufferChar(&out, *p++);
						continue;
					}
					break;
				}
				appendPQExpBufferChar(&out, *p++);
			}
		}
		else if ((p[0] == '-' && p[1] == '-') || (p[0] == '/' && p[1] == '/'))
		{
			while (*p && *p != '\n')
				appendPQExpBufferChar(&out, *p++);
		}
		else if (p[0] == '/' && p[1] == '*')
		{
			appendPQExpBufferChar(&out, *p++);
			appendPQExpBufferChar(&out, *p++);
			while (*p && !(p[0] == '*' && p[1] == '/'))
				appendPQExpBufferChar(&out, *p++);
			if (*p)
			{
				appendPQExpBufferChar(&out, *p++);
				appendPQExpBufferChar(&out, *p++);
			}
		}
		else if (*p == '$' && isdigit((unsigned char) p[1]))
		{
			p++;
			appendPQExpBufferStr(&out, "$p");
			while (isdigit((unsigned char) *p))
				appendPQExpBufferChar(&out, *p++);
		}
		else
		{
			appendPQExpBufferChar(&out, *p++);
		}
	}

	return out.data;
}

static char *
neo4j_prepare_cypher_statement(const char *raw_sql, Neo4jSqlControlType *ctrl_out)
{
	const char *p = raw_sql;
	size_t		len;

	*ctrl_out = NEO4J_CTRL_NONE;
	if (raw_sql == NULL)
	{
		*ctrl_out = NEO4J_CTRL_NOOP;
		return pg_strdup("");
	}

	while (*p && isspace((unsigned char) *p))
		p++;
	len = strlen(p);
	while (len > 0 && (isspace((unsigned char) p[len - 1]) || p[len - 1] == ';'))
		len--;

	if (len == 0)
	{
		*ctrl_out = NEO4J_CTRL_NOOP;
		return pg_strdup("");
	}

	if ((len == 5 && pg_strncasecmp(p, "begin", 5) == 0) ||
		(len > 5 && pg_strncasecmp(p, "begin", 5) == 0 && isspace((unsigned char) p[5])) ||
		(len >= 17 && pg_strncasecmp(p, "start transaction", 17) == 0))
	{
		*ctrl_out = NEO4J_CTRL_BEGIN;
		return pg_strdup("BEGIN");
	}

	if ((len == 6 && pg_strncasecmp(p, "commit", 6) == 0) ||
		(len > 6 && pg_strncasecmp(p, "commit", 6) == 0 && isspace((unsigned char) p[6])) ||
		(len == 3 && pg_strncasecmp(p, "end", 3) == 0))
	{
		*ctrl_out = NEO4J_CTRL_COMMIT;
		return pg_strdup("COMMIT");
	}

	if ((len == 8 && pg_strncasecmp(p, "rollback", 8) == 0) ||
		(len > 8 && pg_strncasecmp(p, "rollback", 8) == 0 && isspace((unsigned char) p[8])) ||
		(len == 5 && pg_strncasecmp(p, "abort", 5) == 0))
	{
		*ctrl_out = NEO4J_CTRL_ROLLBACK;
		return pg_strdup("ROLLBACK");
	}

	if ((len >= 6 && pg_strncasecmp(p, "vacuum", 6) == 0 && (len == 6 || isspace((unsigned char) p[6]))) ||
		(len >= 7 && pg_strncasecmp(p, "analyze", 7) == 0 && (len == 7 || isspace((unsigned char) p[7]))) ||
		(len >= 12 && pg_strncasecmp(p, "create table", 12) == 0) ||
		(len >= 11 && pg_strncasecmp(p, "alter table", 11) == 0) ||
		(len >= 10 && pg_strncasecmp(p, "drop table", 10) == 0) ||
		(len >= 3 && pg_strncasecmp(p, "set", 3) == 0 && isspace((unsigned char) p[3])))
	{
		*ctrl_out = NEO4J_CTRL_NOOP;
		return pg_strdup("");
	}

	if (len >= 8 && pg_strncasecmp(p, "truncate", 8) == 0 && isspace((unsigned char) p[8]))
		return pg_strdup("MATCH (n) DETACH DELETE n");

	if (pg_strncasecmp(p, "select count(*) from pgbench_branches", 37) == 0)
		return pg_strdup("MATCH (b:pgbench_branches) RETURN count(b) AS count");

	if (pg_strncasecmp(p, "select abalance from pgbench_accounts where aid =", 49) == 0)
	{
		char	   *translated = neo4j_translate_dollar_params(p + 49);
		char	   *cypher = psprintf("MATCH (a:pgbench_accounts {aid: %s}) RETURN a.abalance AS abalance",
									  translated);

		pg_free(translated);
		return cypher;
	}

	if (pg_strncasecmp(p, "update pgbench_accounts set abalance = abalance +", 49) == 0)
		return pg_strdup("MATCH (a:pgbench_accounts {aid: $p2}) SET a.abalance = coalesce(a.abalance, 0) + $p1");

	if (pg_strncasecmp(p, "update pgbench_tellers set tbalance = tbalance +", 48) == 0)
		return pg_strdup("MATCH (t:pgbench_tellers {tid: $p2}) SET t.tbalance = coalesce(t.tbalance, 0) + $p1");

	if (pg_strncasecmp(p, "update pgbench_branches set bbalance = bbalance +", 49) == 0)
		return pg_strdup("MATCH (b:pgbench_branches {bid: $p2}) SET b.bbalance = coalesce(b.bbalance, 0) + $p1");

	if (pg_strncasecmp(p, "insert into pgbench_history", 27) == 0)
		return pg_strdup("CREATE (:pgbench_history {tid: $p1, bid: $p2, aid: $p3, delta: $p4, mtime: timestamp()})");

	return neo4j_translate_dollar_params(raw_sql);
}

static bool
neo4j_str_is_canonical_int(const char *s, int64 *ival_out)
{
	char	   *endptr = NULL;
	long long	v;

	if (s == NULL || *s == '\0')
		return false;
	if (s[0] == '-' && s[1] == '\0')
		return false;
	if (s[0] == '0' && s[1] != '\0')
		return false;
	if (s[0] == '-' && s[1] == '0')
		return false;

	errno = 0;
	v = strtoll(s, &endptr, 10);
	if (errno != 0 || endptr == NULL || *endptr != '\0')
		return false;
	*ival_out = (int64) v;
	return true;
}

static bool
neo4j_str_is_canonical_double(const char *s, double *dval_out)
{
	char	   *endptr = NULL;
	double		d;

	if (s == NULL || *s == '\0')
		return false;
	if (strchr(s, '.') == NULL && strchr(s, 'e') == NULL && strchr(s, 'E') == NULL)
		return false;

	errno = 0;
	d = strtod(s, &endptr);
	if (errno != 0 || endptr == NULL || *endptr != '\0')
		return false;
	*dval_out = d;
	return true;
}

static void
neo4j_append_pgbench_value_json(PQExpBuffer buf, const PgBenchValue *v)
{
	if (v == NULL)
	{
		appendPQExpBufferStr(buf, "null");
		return;
	}

	switch (v->type)
	{
		case PGBT_NO_VALUE:
		case PGBT_NULL:
			appendPQExpBufferStr(buf, "null");
			break;
		case PGBT_INT:
			appendPQExpBuffer(buf, INT64_FORMAT, v->u.ival);
			break;
		case PGBT_DOUBLE:
			appendPQExpBuffer(buf, "%.15g", v->u.dval);
			break;
		case PGBT_BOOLEAN:
			appendPQExpBufferStr(buf, v->u.bval ? "true" : "false");
			break;
		case PGBT_TEXT:
			neo4j_append_json_str(buf, v->u.sval ? v->u.sval : "");
			break;
		case PGBT_BYTEA:
			{
				static const char hex[] = "0123456789abcdef";

				appendPQExpBufferStr(buf, "\"\\\\x");
				for (int b = 0; b < v->u.byval.len; b++)
				{
					uint8		c = v->u.byval.data[b];

					appendPQExpBufferChar(buf, hex[(c >> 4) & 0x0F]);
					appendPQExpBufferChar(buf, hex[c & 0x0F]);
				}
				appendPQExpBufferChar(buf, '"');
			}
			break;
		case PGBT_VECTOR:
			appendPQExpBufferChar(buf, '[');
			for (int i = 0; i < v->u.vval.dim; i++)
			{
				if (i > 0)
					appendPQExpBufferChar(buf, ',');
				appendPQExpBuffer(buf, "%.7g", (double) v->u.vval.elems[i]);
			}
			appendPQExpBufferChar(buf, ']');
			break;
		case PGBT_ARRAY:
			appendPQExpBufferChar(buf, '[');
			for (int i = 0; i < v->u.aval.nelems; i++)
			{
				if (i > 0)
					appendPQExpBufferChar(buf, ',');
				neo4j_append_pgbench_value_json(buf, &v->u.aval.elems[i]);
			}
			appendPQExpBufferChar(buf, ']');
			break;
		default:
			appendPQExpBufferStr(buf, "null");
			break;
	}
}

static void
neo4j_append_param_json_value(PQExpBuffer buf, int idx, int nparams,
							  const char *const *param_texts,
							  const struct Variable *const *param_vars)
{
	if (idx < 0 || idx >= nparams)
	{
		appendPQExpBufferStr(buf, "null");
		return;
	}

	if (param_vars != NULL && param_vars[idx] != NULL &&
		param_vars[idx]->value.type != PGBT_NO_VALUE)
	{
		neo4j_append_pgbench_value_json(buf, &param_vars[idx]->value);
		return;
	}

	if (param_texts != NULL && param_texts[idx] != NULL)
	{
		const char *s = param_texts[idx];
		int64		ival;
		double		dval;

		if (neo4j_str_is_canonical_int(s, &ival))
			appendPQExpBuffer(buf, INT64_FORMAT, ival);
		else if (neo4j_str_is_canonical_double(s, &dval))
			appendPQExpBuffer(buf, "%.15g", dval);
		else if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0)
			appendPQExpBufferStr(buf, s);
		else
			neo4j_append_json_str(buf, s);
	}
	else
	{
		appendPQExpBufferStr(buf, "null");
	}
}

static void
neo4j_append_stmt_json_obj(PQExpBuffer buf, const char *cypher, int nparams,
						   const char *const *param_texts,
						   const struct Variable *const *param_vars)
{
	appendPQExpBufferStr(buf, "{\"statement\":");
	neo4j_append_json_str(buf, cypher);
	appendPQExpBufferStr(buf, ",\"parameters\":{");
	for (int i = 0; i < nparams; i++)
	{
		if (i > 0)
			appendPQExpBufferChar(buf, ',');
		appendPQExpBuffer(buf, "\"p%d\":", i + 1);
		neo4j_append_param_json_value(buf, i, nparams, param_texts, param_vars);
	}
	appendPQExpBufferStr(buf, "}}");
}

/*-------------------------------------------------------------------------
 * HTTP Response Parsing & Neo4j Result Set Decoding
 *-------------------------------------------------------------------------
 */

static char *
neo4j_extract_tx_path(const char *url_or_path)
{
	const char *db_pos;
	const char *commit_pos;

	if (url_or_path == NULL || *url_or_path == '\0')
		return NULL;

	db_pos = strstr(url_or_path, "/db/");
	if (db_pos == NULL)
		return NULL;

	commit_pos = strstr(db_pos, "/commit");
	if (commit_pos != NULL)
		return pnstrdup(db_pos, commit_pos - db_pos);

	return pg_strdup(db_pos);
}

static char *
neo4j_json_node_to_cell_str(const Neo4jJsonNode *node, bool *is_null_out)
{
	if (node == NULL || node->type == NJSON_NULL)
	{
		*is_null_out = true;
		return pg_strdup("");
	}

	*is_null_out = false;
	if (node->type == NJSON_STRING || node->type == NJSON_NUMBER || node->type == NJSON_BOOL)
		return pg_strdup(node->str ? node->str : "");

	{
		PQExpBufferData buf;

		initPQExpBuffer(&buf);
		neo4j_serialize_json_node(&buf, node);
		return buf.data;
	}
}

static PgBenchResult *
neo4j_build_result_from_stmt_json(const Neo4jJsonNode *rnode)
{
	Neo4jJsonNode *cols_node = neo4j_json_get_field(rnode, "columns");
	Neo4jJsonNode *data_node = neo4j_json_get_field(rnode, "data");
	PgBenchResult *res;

	if (cols_node == NULL || cols_node->type != NJSON_ARRAY || cols_node->nelems == 0)
		return neo4j_alloc_result(PGBENCH_RES_COMMAND_OK);

	res = neo4j_alloc_result(PGBENCH_RES_TUPLES_OK);
	res->ncols = cols_node->nelems;
	res->col_names = pg_malloc0_array(char *, res->ncols);
	for (int c = 0; c < res->ncols; c++)
	{
		Neo4jJsonNode *cn = cols_node->elems[c];

		res->col_names[c] = pg_strdup((cn && cn->str) ? cn->str : "?column?");
	}

	if (data_node != NULL && data_node->type == NJSON_ARRAY && data_node->nelems > 0)
	{
		res->nrows = data_node->nelems;
		res->cells = pg_malloc0_array(char **, res->nrows);
		res->is_null = pg_malloc0_array(bool *, res->nrows);

		for (int r = 0; r < res->nrows; r++)
		{
			Neo4jJsonNode *ditem = data_node->elems[r];
			Neo4jJsonNode *row_arr = neo4j_json_get_field(ditem, "row");

			res->cells[r] = pg_malloc0_array(char *, res->ncols);
			res->is_null[r] = pg_malloc0_array(bool, res->ncols);

			for (int c = 0; c < res->ncols; c++)
			{
				Neo4jJsonNode *cell_node = (row_arr && row_arr->type == NJSON_ARRAY && c < row_arr->nelems) ?
					row_arr->elems[c] : NULL;

				res->cells[r][c] = neo4j_json_node_to_cell_str(cell_node, &res->is_null[r][c]);
			}
		}
	}

	return res;
}

static void
neo4j_handle_http_response(PgBenchConn *conn, int http_status,
						   const char *location_hdr, const char *body, size_t body_len)
{
	Neo4jPendingHttp *req = conn->pending_head;
	const char *p = body;
	Neo4jJsonNode *root = NULL;
	Neo4jJsonNode *errors_node = NULL;
	Neo4jJsonNode *results_node = NULL;
	Neo4jJsonNode *commit_node = NULL;
	char	   *body_copy = NULL;

	if (req == NULL)
		return;

	if (body != NULL && body_len > 0)
	{
		body_copy = pnstrdup(body, body_len);
		p = body_copy;
		root = neo4j_json_parse_value(&p);
	}

	if (root != NULL && root->type == NJSON_OBJECT)
	{
		errors_node = neo4j_json_get_field(root, "errors");
		results_node = neo4j_json_get_field(root, "results");
		commit_node = neo4j_json_get_field(root, "commit");
	}

	if (req->kind == NEO4J_REQ_BEGIN || req->kind == NEO4J_REQ_IN_TX ||
		req->kind == NEO4J_REQ_PIPELINE_BATCH)
	{
		const char *commit_str = (commit_node && commit_node->type == NJSON_STRING) ?
			commit_node->str : location_hdr;

		if (commit_str != NULL && conn->active_tx_path == NULL &&
			(req->kind == NEO4J_REQ_BEGIN || conn->tx_status == PGBENCH_TX_INTRANS))
		{
			conn->active_tx_path = neo4j_extract_tx_path(commit_str);
		}
	}

	if (req->kind == NEO4J_REQ_COMMIT || req->kind == NEO4J_REQ_ROLLBACK)
	{
		pg_free(conn->active_tx_path);
		conn->active_tx_path = NULL;
		conn->tx_status = PGBENCH_TX_IDLE;
	}

	/*
	 * If a ROLLBACK (DELETE /db/<db>/tx/<id>) was sent for a transaction that
	 * the server had already auto-aborted on deadlock/transient error, treat
	 * HTTP 404 or TransactionNotFound as a clean rollback completion.
	 */
	if (req->kind == NEO4J_REQ_ROLLBACK)
	{
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
		neo4j_json_free(root);
		pg_free(body_copy);
		neo4j_pop_pending(conn);
		return;
	}

	if ((errors_node != NULL && errors_node->type == NJSON_ARRAY && errors_node->nelems > 0) ||
		http_status >= 400)
	{
		const char *err_code = "Neo.DatabaseError.General.UnknownError";
		const char *err_msg = "Neo4j HTTP query error";
		char		sqlstate[6] = "XX000";
		PgBenchResult *err_res;
		int			produced = 0;

		if (errors_node != NULL && errors_node->type == NJSON_ARRAY && errors_node->nelems > 0)
		{
			Neo4jJsonNode *e0 = errors_node->elems[0];
			Neo4jJsonNode *f_code = neo4j_json_get_field(e0, "code");
			Neo4jJsonNode *f_msg = neo4j_json_get_field(e0, "message");

			if (f_code && f_code->str)
				err_code = f_code->str;
			if (f_msg && f_msg->str)
				err_msg = f_msg->str;
		}

		/*
		 * Map Neo.TransientError.* (deadlocks, lock client stopped, outdated)
		 * or HTTP 409 Conflict to SQLSTATE "40001" for pgbench retry!
		 */
		if (strstr(err_code, "TransientError") != NULL ||
			strstr(err_code, "DeadlockDetected") != NULL ||
			strstr(err_code, "LockClientStopped") != NULL ||
			http_status == 409)
		{
			strcpy(sqlstate, "40001");
		}

		if (conn->tx_status == PGBENCH_TX_INTRANS)
		{
			/*
			 * Keep conn->active_tx_path so the subsequent ROLLBACK command
			 * dispatches DELETE /db/<db>/tx/<id> to the server.
			 */
			conn->tx_status = PGBENCH_TX_INERROR;
		}

		if (results_node != NULL && results_node->type == NJSON_ARRAY)
		{
			for (int i = 0; i < results_node->nelems; i++)
			{
				neo4j_push_result(conn, neo4j_build_result_from_stmt_json(results_node->elems[i]));
				produced++;
			}
		}

		err_res = neo4j_alloc_result(PGBENCH_RES_FATAL_ERROR);
		memcpy(err_res->sqlstate, sqlstate, 6);
		err_res->err_msg = psprintf("ERROR (%s) [%s]: %s\n", sqlstate, err_code, err_msg);
		printfPQExpBuffer(&conn->err_buf, "%s", err_res->err_msg);
		neo4j_push_result(conn, err_res);
		produced++;

		if (conn->pipe_status != PGBENCH_PIPE_OFF)
		{
			conn->pipeline_aborted = true;
			conn->pipe_status = PGBENCH_PIPE_ABORTED;
			while (produced < req->expected_stmts)
			{
				neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_PIPELINE_ABORTED));
				produced++;
			}
		}

		if (req->has_pipeline_sync)
		{
			conn->pipeline_aborted = false;
			if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
				conn->pipe_status = PGBENCH_PIPE_ON;
			neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_PIPELINE_SYNC));
		}

		neo4j_json_free(root);
		pg_free(body_copy);
		neo4j_pop_pending(conn);
		return;
	}

	if (req->kind == NEO4J_REQ_BEGIN)
	{
		conn->tx_status = PGBENCH_TX_INTRANS;
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
	}
	else if (req->kind == NEO4J_REQ_COMMIT || req->kind == NEO4J_REQ_ROLLBACK)
	{
		conn->tx_status = PGBENCH_TX_IDLE;
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
	}
	else
	{
		int			produced = 0;

		if (results_node != NULL && results_node->type == NJSON_ARRAY)
		{
			for (int i = 0; i < results_node->nelems; i++)
			{
				neo4j_push_result(conn, neo4j_build_result_from_stmt_json(results_node->elems[i]));
				produced++;
			}
		}
		while (produced < req->expected_stmts)
		{
			neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
			produced++;
		}
	}

	if (req->has_pipeline_sync)
	{
		conn->pipeline_aborted = false;
		if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
			conn->pipe_status = PGBENCH_PIPE_ON;
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_PIPELINE_SYNC));
	}

	neo4j_json_free(root);
	pg_free(body_copy);
	neo4j_pop_pending(conn);
}

static ssize_t
neo4j_decode_chunked_body(const char *src, size_t src_len, PQExpBuffer decoded_out)
{
	const char *p = src;
	const char *end = src + src_len;

	resetPQExpBuffer(decoded_out);
	for (;;)
	{
		const char *crlf = memmem(p, end - p, "\r\n", 2);
		char		hex_buf[32];
		size_t		hlen;
		size_t		chunk_sz;

		if (crlf == NULL)
			return -1;
		hlen = (size_t) (crlf - p);
		if (hlen == 0 || hlen >= sizeof(hex_buf))
			return -1;
		memcpy(hex_buf, p, hlen);
		hex_buf[hlen] = '\0';
		chunk_sz = (size_t) strtoul(hex_buf, NULL, 16);
		p = crlf + 2;

		if (chunk_sz == 0)
		{
			if (end - p < 2)
				return -1;
			p += 2;
			return (ssize_t) (p - src);
		}

		if ((size_t) (end - p) < chunk_sz + 2)
			return -1;
		appendBinaryPQExpBuffer(decoded_out, p, chunk_sz);
		p += chunk_sz + 2;
	}
}

static void
neo4j_try_parse_http_responses(PgBenchConn *conn)
{
	while (conn->in_len > 0 && conn->pending_head != NULL)
	{
		const char *hdr_end = memmem(conn->in_buf, conn->in_len, "\r\n\r\n", 4);
		size_t		hdr_len;
		int			http_status = 200;
		long		content_len = -1;
		bool		is_chunked = false;
		char	   *location_hdr = NULL;
		const char *line;
		const char *body_start;
		size_t		avail_body;

		if (hdr_end == NULL)
			return;

		hdr_len = (size_t) (hdr_end - conn->in_buf) + 4;
		body_start = conn->in_buf + hdr_len;
		avail_body = conn->in_len - hdr_len;

		if (sscanf(conn->in_buf, "HTTP/%*d.%*d %d", &http_status) != 1)
			http_status = 200;

		line = conn->in_buf;
		while (line < hdr_end)
		{
			const char *next_line = memmem(line, hdr_end - line, "\r\n", 2);
			size_t		llen = next_line ? (size_t) (next_line - line) : (size_t) (hdr_end - line);

			if (llen > 15 && pg_strncasecmp(line, "Content-Length:", 15) == 0)
			{
				content_len = strtol(line + 15, NULL, 10);
			}
			else if (llen > 18 && pg_strncasecmp(line, "Transfer-Encoding:", 18) == 0)
			{
				char	   *tmp = pnstrdup(line + 18, llen - 18);

				if (strstr(tmp, "chunked") != NULL || strstr(tmp, "Chunked") != NULL)
					is_chunked = true;
				pg_free(tmp);
			}
			else if (llen > 9 && pg_strncasecmp(line, "Location:", 9) == 0)
			{
				const char *vs = line + 9;
				const char *ve = line + llen;

				while (vs < ve && isspace((unsigned char) *vs))
					vs++;
				pg_free(location_hdr);
				location_hdr = pnstrdup(vs, ve - vs);
			}

			if (next_line == NULL)
				break;
			line = next_line + 2;
		}

		if (is_chunked)
		{
			PQExpBufferData decoded;
			ssize_t		consumed;

			initPQExpBuffer(&decoded);
			consumed = neo4j_decode_chunked_body(body_start, avail_body, &decoded);
			if (consumed < 0)
			{
				termPQExpBuffer(&decoded);
				pg_free(location_hdr);
				return;
			}

			neo4j_handle_http_response(conn, http_status, location_hdr,
									   decoded.data, decoded.len);
			termPQExpBuffer(&decoded);
			pg_free(location_hdr);

			{
				size_t		total_consumed = hdr_len + (size_t) consumed;
				size_t		rem = conn->in_len - total_consumed;

				if (rem > 0)
					memmove(conn->in_buf, conn->in_buf + total_consumed, rem);
				conn->in_len = rem;
			}
		}
		else if (content_len >= 0)
		{
			size_t		total_needed = hdr_len + (size_t) content_len;

			if (conn->in_len < total_needed)
			{
				pg_free(location_hdr);
				return;
			}

			neo4j_handle_http_response(conn, http_status, location_hdr,
									   body_start, (size_t) content_len);
			pg_free(location_hdr);

			{
				size_t		rem = conn->in_len - total_needed;

				if (rem > 0)
					memmove(conn->in_buf, conn->in_buf + total_needed, rem);
				conn->in_len = rem;
			}
		}
		else
		{
			neo4j_handle_http_response(conn, http_status, location_hdr, NULL, 0);
			pg_free(location_hdr);

			{
				size_t		rem = conn->in_len - hdr_len;

				if (rem > 0)
					memmove(conn->in_buf, conn->in_buf + hdr_len, rem);
				conn->in_len = rem;
			}
		}
	}
}

/*-------------------------------------------------------------------------
 * Connection Lifecycle
 *-------------------------------------------------------------------------
 */

static bool
neo4j_init_global(const char *conninfo, int nthreads_arg, int nclients_arg)
{
	(void) nthreads_arg;
	(void) nclients_arg;
	pg_free(saved_neo4j_conninfo);
	saved_neo4j_conninfo = conninfo ? pg_strdup(conninfo) : NULL;
	return true;
}

static void
neo4j_shutdown_global(void)
{
	pg_free(saved_neo4j_conninfo);
	saved_neo4j_conninfo = NULL;
}

static void
neo4j_set_cancel_conn(PgBenchConn *conn)
{
	(void) conn;
}

static void
neo4j_reset_cancel_conn(void)
{
}

static PgBenchConn *
neo4j_connect_start(const char *host, const char *port,
					const char *user, const char *dbname)
{
	PgBenchConn *conn = pg_malloc0_object(PgBenchConn);
	const char *env_val;

	conn->sock = -1;
	conn->status = PGBENCH_CONN_BAD;
	conn->tx_status = PGBENCH_TX_IDLE;
	conn->pipe_status = PGBENCH_PIPE_OFF;
	initPQExpBuffer(&conn->err_buf);
	initPQExpBuffer(&conn->pipe_stmts_buf);

	env_val = getenv("NEO4J_HOST");
	conn->host = pg_strdup((env_val && *env_val) ? env_val : "127.0.0.1");

	env_val = getenv("NEO4J_PORT");
	conn->port = pg_strdup((env_val && *env_val) ? env_val : NEO4J_DEFAULT_PORT);

	env_val = getenv("NEO4J_USER");
	conn->user = pg_strdup((env_val && *env_val) ? env_val : "neo4j");

	env_val = getenv("NEO4J_PASSWORD");
	conn->password = pg_strdup(env_val ? env_val : "");

	env_val = getenv("NEO4J_DATABASE");
	conn->dbname = pg_strdup((env_val && *env_val) ? env_val : NEO4J_DEFAULT_DB);

	env_val = getenv("NEO4J_URI");
	if (env_val && *env_val)
		neo4j_parse_uri(env_val, &conn->use_tls, &conn->host, &conn->port,
						&conn->user, &conn->password, &conn->dbname);

	if (saved_neo4j_conninfo && *saved_neo4j_conninfo)
		neo4j_parse_uri(saved_neo4j_conninfo, &conn->use_tls, &conn->host, &conn->port,
						&conn->user, &conn->password, &conn->dbname);

	if (dbname && *dbname)
	{
		if (pg_strncasecmp(dbname, "neo4j://", 8) == 0 ||
			pg_strncasecmp(dbname, "neo4j+s://", 10) == 0 ||
			pg_strncasecmp(dbname, "neo4j+ssc://", 12) == 0 ||
			pg_strncasecmp(dbname, "bolt://", 7) == 0 ||
			pg_strncasecmp(dbname, "bolt+s://", 9) == 0 ||
			pg_strncasecmp(dbname, "bolt+ssc://", 11) == 0 ||
			pg_strncasecmp(dbname, "http://", 7) == 0 ||
			pg_strncasecmp(dbname, "https://", 8) == 0)
		{
			neo4j_parse_uri(dbname, &conn->use_tls, &conn->host, &conn->port,
							&conn->user, &conn->password, &conn->dbname);
		}
		else
		{
			pg_free(conn->dbname);
			conn->dbname = pg_strdup(dbname);
		}
	}

	if (host && *host)
	{
		if (pg_strncasecmp(host, "neo4j://", 8) == 0 ||
			pg_strncasecmp(host, "neo4j+s://", 10) == 0 ||
			pg_strncasecmp(host, "neo4j+ssc://", 12) == 0 ||
			pg_strncasecmp(host, "bolt://", 7) == 0 ||
			pg_strncasecmp(host, "bolt+s://", 9) == 0 ||
			pg_strncasecmp(host, "bolt+ssc://", 11) == 0 ||
			pg_strncasecmp(host, "http://", 7) == 0 ||
			pg_strncasecmp(host, "https://", 8) == 0)
		{
			neo4j_parse_uri(host, &conn->use_tls, &conn->host, &conn->port,
							&conn->user, &conn->password, &conn->dbname);
		}
		else
		{
			pg_free(conn->host);
			conn->host = pg_strdup(host);
		}
	}

	if (port && *port)
	{
		pg_free(conn->port);
		conn->port = pg_strdup(port);
	}

	if (user && *user)
	{
		pg_free(conn->user);
		conn->user = pg_strdup(user);
	}

	if (conn->dbname == NULL || conn->dbname[0] == '\0')
	{
		pg_free(conn->dbname);
		conn->dbname = pg_strdup(NEO4J_DEFAULT_DB);
	}

	env_val = getenv("NEO4J_AUTH_TOKEN");
	if (env_val && *env_val)
	{
		conn->auth_header = psprintf("Bearer %s", env_val);
	}
	else if (conn->user && conn->user[0] != '\0')
	{
		char	   *cred = psprintf("%s:%s", conn->user, conn->password ? conn->password : "");
		char	   *b64 = neo4j_base64_encode((const uint8 *) cred, strlen(cred));

		conn->auth_header = psprintf("Basic %s", b64);
		pg_free(b64);
		pg_free(cred);
	}

	if (neo4j_open_socket(conn, false))
		conn->status = PGBENCH_CONN_OK;

	return conn;
}

static PgBenchConnStatus
neo4j_connect_poll(PgBenchConn *conn)
{
	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return PGBENCH_CONN_POLLING_FAILED;
	return PGBENCH_CONN_POLLING_OK;
}

static PgBenchConn *
neo4j_connect_sync(const char *host, const char *port,
				   const char *user, const char *dbname)
{
	return neo4j_connect_start(host, port, user, dbname);
}

static void
neo4j_finish(PgBenchConn *conn)
{
	if (conn == NULL)
		return;

	if (conn->sock >= 0)
	{
#ifdef USE_OPENSSL
		if (conn->ssl)
			SSL_free(conn->ssl);
		if (conn->ssl_ctx)
			SSL_CTX_free(conn->ssl_ctx);
#endif
		close(conn->sock);
		conn->sock = -1;
	}

	while (conn->pending_head)
		neo4j_pop_pending(conn);

	while (conn->res_head)
	{
		PgBenchResult *r = conn->res_head;

		conn->res_head = r->next;
		neo4j_free_result(r);
	}
	conn->res_tail = NULL;

	for (int i = 0; i < conn->nprep_stmts; i++)
	{
		pg_free(conn->prep_stmts[i].name);
		pg_free(conn->prep_stmts[i].cypher);
	}
	pg_free(conn->prep_stmts);

	pg_free(conn->active_tx_path);
	pg_free(conn->host);
	pg_free(conn->port);
	pg_free(conn->dbname);
	pg_free(conn->user);
	pg_free(conn->password);
	pg_free(conn->auth_header);
	pg_free(conn->out_buf);
	pg_free(conn->in_buf);
	termPQExpBuffer(&conn->err_buf);
	termPQExpBuffer(&conn->pipe_stmts_buf);
	pg_free(conn);
}

static PgBenchConnStatus
neo4j_status(const PgBenchConn *conn)
{
	return conn ? conn->status : PGBENCH_CONN_BAD;
}

static const char *
neo4j_error_message(const PgBenchConn *conn)
{
	return (conn && conn->err_buf.data) ? conn->err_buf.data : "";
}

static int
neo4j_get_poll_fd(const PgBenchConn *conn)
{
	return conn ? conn->sock : PGBENCH_POLL_ERROR;
}

static PgBenchTxStatus
neo4j_tx_status(const PgBenchConn *conn)
{
	return conn ? conn->tx_status : PGBENCH_TX_UNKNOWN;
}

static const char *
neo4j_get_host(const PgBenchConn *conn)
{
	return (conn && conn->host) ? conn->host : "";
}

static const char *
neo4j_get_port(const PgBenchConn *conn)
{
	return (conn && conn->port) ? conn->port : "";
}

static const char *
neo4j_get_db(const PgBenchConn *conn)
{
	return (conn && conn->dbname) ? conn->dbname : "";
}

/*-------------------------------------------------------------------------
 * Query & Prepared Statement Dispatch
 *-------------------------------------------------------------------------
 */

static Neo4jPreparedStmt *
neo4j_find_prepared(PgBenchConn *conn, const char *stmt_name)
{
	const char *lookup = stmt_name ? stmt_name : "";

	for (int i = 0; i < conn->nprep_stmts; i++)
	{
		if (strcmp(conn->prep_stmts[i].name, lookup) == 0)
			return &conn->prep_stmts[i];
	}
	return NULL;
}

static void
neo4j_store_prepared(PgBenchConn *conn, const char *stmt_name, const char *raw_sql)
{
	const char *lookup = stmt_name ? stmt_name : "";
	Neo4jPreparedStmt *entry = neo4j_find_prepared(conn, lookup);

	if (entry == NULL)
	{
		if (conn->nprep_stmts >= conn->prep_cap)
		{
			conn->prep_cap = conn->prep_cap ? conn->prep_cap * 2 : 16;
			conn->prep_stmts = pg_realloc_array(conn->prep_stmts,
												Neo4jPreparedStmt,
												conn->prep_cap);
		}
		entry = &conn->prep_stmts[conn->nprep_stmts++];
		entry->name = pg_strdup(lookup);
		entry->cypher = pg_strdup(raw_sql ? raw_sql : "");
	}
	else
	{
		pg_free(entry->cypher);
		entry->cypher = pg_strdup(raw_sql ? raw_sql : "");
	}
}

static bool
neo4j_flush_pipeline_batch(PgBenchConn *conn, bool with_sync)
{
	PQExpBufferData body;
	char	   *path;
	bool		ok;
	int			nstmts = conn->pipe_stmts_count;

	if (nstmts == 0)
	{
		if (with_sync)
		{
			conn->pipeline_aborted = false;
			if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
				conn->pipe_status = PGBENCH_PIPE_ON;
			neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_PIPELINE_SYNC));
		}
		return true;
	}

	initPQExpBuffer(&body);
	appendPQExpBuffer(&body, "{\"statements\":[%s]}", conn->pipe_stmts_buf.data);

	resetPQExpBuffer(&conn->pipe_stmts_buf);
	conn->pipe_stmts_count = 0;

	if (conn->tx_status == PGBENCH_TX_INTRANS && conn->active_tx_path != NULL)
		path = pg_strdup(conn->active_tx_path);
	else
		path = psprintf("/db/%s/tx/commit", conn->dbname);

	ok = neo4j_send_http_request(conn, "POST", path, body.data,
								 NEO4J_REQ_PIPELINE_BATCH, nstmts, with_sync);
	pg_free(path);
	termPQExpBuffer(&body);
	return ok;
}

static bool
neo4j_dispatch_statement(PgBenchConn *conn, const char *raw_sql,
						 int nparams, const char *const *param_texts,
						 const struct Variable *const *param_vars)
{
	Neo4jSqlControlType ctrl = NEO4J_CTRL_NONE;
	char	   *cypher;
	PQExpBufferData stmt_json;
	PQExpBufferData body;
	char	   *path;
	bool		ok;

	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	if (conn->pipe_status != PGBENCH_PIPE_OFF && conn->pipeline_aborted)
	{
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_PIPELINE_ABORTED));
		return true;
	}

	cypher = neo4j_prepare_cypher_statement(raw_sql, &ctrl);

	if (ctrl == NEO4J_CTRL_NOOP)
	{
		pg_free(cypher);
		if (conn->pipe_status != PGBENCH_PIPE_OFF)
		{
			if (conn->pipe_stmts_count > 0)
				appendPQExpBufferChar(&conn->pipe_stmts_buf, ',');
			appendPQExpBufferStr(&conn->pipe_stmts_buf,
								 "{\"statement\":\"RETURN 1\",\"parameters\":{}}");
			conn->pipe_stmts_count++;
			return true;
		}
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
		return true;
	}

	if (ctrl == NEO4J_CTRL_BEGIN)
	{
		pg_free(cypher);
		pg_free(conn->active_tx_path);
		conn->active_tx_path = NULL;
		path = psprintf("/db/%s/tx", conn->dbname);
		ok = neo4j_send_http_request(conn, "POST", path, "{\"statements\":[]}",
									 NEO4J_REQ_BEGIN, 1, false);
		pg_free(path);
		return ok;
	}

	if (ctrl == NEO4J_CTRL_COMMIT)
	{
		pg_free(cypher);
		if (conn->pipe_stmts_count > 0)
		{
			if (!neo4j_flush_pipeline_batch(conn, false))
				return false;
		}
		if (conn->active_tx_path != NULL)
		{
			path = psprintf("%s/commit", conn->active_tx_path);
			ok = neo4j_send_http_request(conn, "POST", path, "{\"statements\":[]}",
										 NEO4J_REQ_COMMIT, 1, false);
			pg_free(path);
			return ok;
		}
		conn->tx_status = PGBENCH_TX_IDLE;
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
		return true;
	}

	if (ctrl == NEO4J_CTRL_ROLLBACK)
	{
		pg_free(cypher);
		resetPQExpBuffer(&conn->pipe_stmts_buf);
		conn->pipe_stmts_count = 0;

		if (conn->active_tx_path != NULL)
		{
			path = pg_strdup(conn->active_tx_path);
			ok = neo4j_send_http_request(conn, "DELETE", path, NULL,
										 NEO4J_REQ_ROLLBACK, 1, false);
			pg_free(path);
			return ok;
		}
		conn->tx_status = PGBENCH_TX_IDLE;
		neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
		return true;
	}

	initPQExpBuffer(&stmt_json);
	neo4j_append_stmt_json_obj(&stmt_json, cypher, nparams, param_texts, param_vars);
	pg_free(cypher);

	if (conn->pipe_status != PGBENCH_PIPE_OFF)
	{
		if (conn->pipe_stmts_count > 0)
			appendPQExpBufferChar(&conn->pipe_stmts_buf, ',');
		appendBinaryPQExpBuffer(&conn->pipe_stmts_buf, stmt_json.data, stmt_json.len);
		conn->pipe_stmts_count++;
		termPQExpBuffer(&stmt_json);
		return true;
	}

	initPQExpBuffer(&body);
	appendPQExpBuffer(&body, "{\"statements\":[%s]}", stmt_json.data);
	termPQExpBuffer(&stmt_json);

	if (conn->tx_status == PGBENCH_TX_INTRANS && conn->active_tx_path != NULL)
	{
		path = pg_strdup(conn->active_tx_path);
		ok = neo4j_send_http_request(conn, "POST", path, body.data,
									 NEO4J_REQ_IN_TX, 1, false);
	}
	else
	{
		path = psprintf("/db/%s/tx/commit", conn->dbname);
		ok = neo4j_send_http_request(conn, "POST", path, body.data,
									 NEO4J_REQ_AUTOCOMMIT, 1, false);
	}

	pg_free(path);
	termPQExpBuffer(&body);
	return ok;
}

static bool
neo4j_send_query(PgBenchConn *conn, const char *sql)
{
	return neo4j_dispatch_statement(conn, sql, 0, NULL, NULL);
}

static bool
neo4j_send_query_params(PgBenchConn *conn, const char *sql, int nparams,
						const char *const *param_texts,
						const struct Variable *const *param_vars)
{
	return neo4j_dispatch_statement(conn, sql, nparams, param_texts, param_vars);
}

static bool
neo4j_send_prepare(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	(void) nparams;
	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	neo4j_store_prepared(conn, stmt_name, sql);
	neo4j_push_result(conn, neo4j_alloc_result(PGBENCH_RES_COMMAND_OK));
	return true;
}

static bool
neo4j_send_query_prepared(PgBenchConn *conn, const char *stmt_name, int nparams,
						  const char *const *param_texts,
						  const struct Variable *const *param_vars)
{
	Neo4jPreparedStmt *stmt;

	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	stmt = neo4j_find_prepared(conn, stmt_name);
	if (stmt == NULL)
	{
		PgBenchResult *err_res = neo4j_alloc_result(PGBENCH_RES_FATAL_ERROR);

		strcpy(err_res->sqlstate, "26000");
		err_res->err_msg = psprintf("prepared statement \"%s\" does not exist\n",
									stmt_name ? stmt_name : "");
		printfPQExpBuffer(&conn->err_buf, "%s", err_res->err_msg);
		neo4j_push_result(conn, err_res);
		return true;
	}

	return neo4j_dispatch_statement(conn, stmt->cypher, nparams, param_texts, param_vars);
}

static bool
neo4j_consume_input(PgBenchConn *conn)
{
	if (conn == NULL || conn->sock < 0)
		return false;

	if (conn->out_sent < conn->out_len)
	{
		if (!neo4j_flush_out(conn))
			return false;
	}

	for (;;)
	{
		ssize_t		n;

		if (conn->in_len + 8192 > conn->in_cap)
		{
			size_t		new_cap = conn->in_cap ? conn->in_cap * 2 : 16384;

			conn->in_buf = pg_realloc(conn->in_buf, new_cap);
			conn->in_cap = new_cap;
		}

#ifdef USE_OPENSSL
		if (conn->use_tls && conn->ssl)
		{
			n = SSL_read(conn->ssl, conn->in_buf + conn->in_len,
						 (int) (conn->in_cap - conn->in_len - 1));
			if (n > 0)
			{
				conn->in_len += (size_t) n;
				conn->in_buf[conn->in_len] = '\0';
				neo4j_try_parse_http_responses(conn);
				continue;
			}
			else
			{
				int			serr = SSL_get_error(conn->ssl, (int) n);

				if (serr == SSL_ERROR_WANT_READ || serr == SSL_ERROR_WANT_WRITE)
					break;
				if (serr == SSL_ERROR_ZERO_RETURN)
					break;
				printfPQExpBuffer(&conn->err_buf, "Neo4j SSL_read failed\n");
				conn->status = PGBENCH_CONN_BAD;
				return false;
			}
		}
#endif

		n = recv(conn->sock, conn->in_buf + conn->in_len,
				 conn->in_cap - conn->in_len - 1, 0);
		if (n > 0)
		{
			conn->in_len += (size_t) n;
			conn->in_buf[conn->in_len] = '\0';
			neo4j_try_parse_http_responses(conn);
		}
		else if (n == 0)
		{
			if (conn->pending_head == NULL)
				break;
			printfPQExpBuffer(&conn->err_buf,
							  "Neo4j server closed HTTP connection before response completed\n");
			conn->status = PGBENCH_CONN_BAD;
			return false;
		}
		else if (errno == EINTR)
		{
			continue;
		}
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			break;
		}
		else
		{
			printfPQExpBuffer(&conn->err_buf,
							  "Neo4j socket recv failed: %s\n", strerror(errno));
			conn->status = PGBENCH_CONN_BAD;
			return false;
		}
	}

	return true;
}

static bool
neo4j_is_busy(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	if (conn->out_sent < conn->out_len)
		(void) neo4j_flush_out(conn);
	if (conn->res_head != NULL)
		return false;
	if (conn->pending_head != NULL && !conn->result_drained)
		return true;
	return false;
}

static PgBenchResult *
neo4j_get_result(PgBenchConn *conn)
{
	if (conn == NULL)
		return NULL;

	if (conn->result_drained)
	{
		conn->result_drained = false;
		return NULL;
	}

	while (conn->res_head == NULL && conn->pending_head != NULL &&
		   conn->status == PGBENCH_CONN_OK)
	{
		struct pollfd pfd;
		int			rc;

		if (conn->out_sent < conn->out_len)
		{
			if (!neo4j_flush_out(conn))
				break;
		}

		pfd.fd = conn->sock;
		pfd.events = POLLIN | ((conn->out_sent < conn->out_len) ? POLLOUT : 0);
		pfd.revents = 0;

		rc = poll(&pfd, 1, 10000);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			conn->status = PGBENCH_CONN_BAD;
			break;
		}
		if (rc == 0)
		{
			printfPQExpBuffer(&conn->err_buf, "Neo4j HTTP request timed out\n");
			conn->status = PGBENCH_CONN_BAD;
			break;
		}
		if (!neo4j_consume_input(conn))
			break;
	}

	if (conn->res_head != NULL)
	{
		PgBenchResult *res = conn->res_head;

		conn->res_head = res->next;
		if (conn->res_head == NULL)
			conn->res_tail = NULL;
		res->next = NULL;
		conn->result_drained = true;
		return res;
	}

	return NULL;
}

static PgBenchResult *
neo4j_exec_sync(PgBenchConn *conn, const char *sql)
{
	PgBenchResult *res;

	if (!neo4j_send_query(conn, sql))
		return neo4j_alloc_result(PGBENCH_RES_FATAL_ERROR);

	res = neo4j_get_result(conn);
	conn->result_drained = false;
	return res;
}

static PgBenchResult *
neo4j_prepare_sync(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	PgBenchResult *res;

	if (!neo4j_send_prepare(conn, stmt_name, sql, nparams))
		return neo4j_alloc_result(PGBENCH_RES_FATAL_ERROR);

	res = neo4j_get_result(conn);
	conn->result_drained = false;
	return res;
}

static char *
neo4j_escape_identifier(PgBenchConn *conn, const char *str, size_t len)
{
	PQExpBufferData buf;

	(void) conn;
	initPQExpBuffer(&buf);
	appendPQExpBufferChar(&buf, '`');
	for (size_t i = 0; i < len; i++)
	{
		if (str[i] == '`')
			appendPQExpBufferStr(&buf, "``");
		else
			appendPQExpBufferChar(&buf, str[i]);
	}
	appendPQExpBufferChar(&buf, '`');
	return buf.data;
}

static void
neo4j_free_mem(void *ptr)
{
	pg_free(ptr);
}

/*-------------------------------------------------------------------------
 * Pipeline Mode
 *-------------------------------------------------------------------------
 */

static bool
neo4j_enter_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;
	conn->pipe_status = PGBENCH_PIPE_ON;
	conn->pipeline_aborted = false;
	resetPQExpBuffer(&conn->pipe_stmts_buf);
	conn->pipe_stmts_count = 0;
	return true;
}

static bool
neo4j_exit_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	if (conn->pipe_stmts_count > 0 || conn->pending_head != NULL || conn->res_head != NULL)
		return false;
	conn->pipe_status = PGBENCH_PIPE_OFF;
	conn->pipeline_aborted = false;
	conn->result_drained = false;
	return true;
}

static PgBenchPipelineStatus
neo4j_pipeline_status(const PgBenchConn *conn)
{
	return conn ? conn->pipe_status : PGBENCH_PIPE_OFF;
}

static bool
neo4j_send_pipeline_sync(PgBenchConn *conn)
{
	if (conn == NULL || conn->pipe_status == PGBENCH_PIPE_OFF)
		return false;
	return neo4j_flush_pipeline_batch(conn, true);
}

static bool
neo4j_pipeline_sync(PgBenchConn *conn)
{
	return neo4j_send_pipeline_sync(conn);
}

/*-------------------------------------------------------------------------
 * Result Inspection
 *-------------------------------------------------------------------------
 */

static PgBenchExecStatus
neo4j_result_status(const PgBenchResult *res)
{
	return res ? res->status : PGBENCH_RES_FATAL_ERROR;
}

static const char *
neo4j_result_error_field(const PgBenchResult *res, int fieldcode)
{
	(void) fieldcode;
	if (res == NULL || res->sqlstate[0] == '\0')
		return NULL;
	return res->sqlstate;
}

static const char *
neo4j_result_error_message(const PgBenchResult *res, const PgBenchConn *conn)
{
	if (res && res->err_msg)
		return res->err_msg;
	if (conn && conn->err_buf.data)
		return conn->err_buf.data;
	return "";
}

static int
neo4j_ntuples(const PgBenchResult *res)
{
	return res ? res->nrows : 0;
}

static int
neo4j_nfields(const PgBenchResult *res)
{
	return res ? res->ncols : 0;
}

static const char *
neo4j_fname(const PgBenchResult *res, int col)
{
	if (res == NULL || col < 0 || col >= res->ncols || res->col_names == NULL)
		return NULL;
	return res->col_names[col];
}

static int
neo4j_fnumber(const PgBenchResult *res, const char *col_name)
{
	if (res == NULL || col_name == NULL || res->col_names == NULL)
		return -1;
	for (int c = 0; c < res->ncols; c++)
	{
		if (res->col_names[c] && pg_strcasecmp(res->col_names[c], col_name) == 0)
			return c;
	}
	return -1;
}

static bool
neo4j_get_is_null(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->is_null == NULL)
		return true;
	return res->is_null[row][col];
}

static const char *
neo4j_get_value(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return "";
	return res->cells[row][col] ? res->cells[row][col] : "";
}

static void
neo4j_clear_result(PgBenchResult *res)
{
	neo4j_free_result(res);
}

/*-------------------------------------------------------------------------
 * Bulk Loading (copy_begin, copy_put_data, copy_put_row, copy_end)
 *-------------------------------------------------------------------------
 */

static void
neo4j_populate_default_cols(PgBenchCopyStream *stream, const char *table_name)
{
	if (pg_strcasecmp(table_name, "pgbench_branches") == 0)
	{
		stream->ncols = 3;
		stream->col_names = pg_malloc0_array(char *, 3);
		stream->col_names[0] = pg_strdup("bid");
		stream->col_names[1] = pg_strdup("bbalance");
		stream->col_names[2] = pg_strdup("filler");
	}
	else if (pg_strcasecmp(table_name, "pgbench_tellers") == 0)
	{
		stream->ncols = 4;
		stream->col_names = pg_malloc0_array(char *, 4);
		stream->col_names[0] = pg_strdup("tid");
		stream->col_names[1] = pg_strdup("bid");
		stream->col_names[2] = pg_strdup("tbalance");
		stream->col_names[3] = pg_strdup("filler");
	}
	else if (pg_strcasecmp(table_name, "pgbench_accounts") == 0)
	{
		stream->ncols = 4;
		stream->col_names = pg_malloc0_array(char *, 4);
		stream->col_names[0] = pg_strdup("aid");
		stream->col_names[1] = pg_strdup("bid");
		stream->col_names[2] = pg_strdup("abalance");
		stream->col_names[3] = pg_strdup("filler");
	}
	else if (pg_strcasecmp(table_name, "pgbench_history") == 0)
	{
		stream->ncols = 6;
		stream->col_names = pg_malloc0_array(char *, 6);
		stream->col_names[0] = pg_strdup("tid");
		stream->col_names[1] = pg_strdup("bid");
		stream->col_names[2] = pg_strdup("aid");
		stream->col_names[3] = pg_strdup("delta");
		stream->col_names[4] = pg_strdup("mtime");
		stream->col_names[5] = pg_strdup("filler");
	}
}

static bool
neo4j_flush_copy_batch(PgBenchCopyStream *stream)
{
	PQExpBufferData body;
	char	   *cypher;
	char	   *path;
	PgBenchResult *res;
	bool		ok;
	PgBenchConn *conn = stream->conn;

	if (stream->buffered_rows == 0)
		return true;

	cypher = psprintf("UNWIND $rows AS row CREATE (n:`%s`) SET n = row", stream->table_name);
	initPQExpBuffer(&body);
	appendPQExpBufferStr(&body, "{\"statements\":[{\"statement\":");
	neo4j_append_json_str(&body, cypher);
	appendPQExpBuffer(&body, ",\"parameters\":{\"rows\":[%s]}}]}",
					  stream->rows_json.data);
	pg_free(cypher);

	resetPQExpBuffer(&stream->rows_json);
	stream->buffered_rows = 0;

	if (conn->tx_status == PGBENCH_TX_INTRANS && conn->active_tx_path != NULL)
		path = pg_strdup(conn->active_tx_path);
	else
		path = psprintf("/db/%s/tx/commit", conn->dbname);

	ok = neo4j_send_http_request(conn, "POST", path, body.data,
								 NEO4J_REQ_AUTOCOMMIT, 1, false);
	pg_free(path);
	termPQExpBuffer(&body);

	if (!ok)
	{
		stream->failed = true;
		return false;
	}

	res = neo4j_get_result(conn);
	conn->result_drained = false;
	ok = (res != NULL && (res->status == PGBENCH_RES_COMMAND_OK ||
						  res->status == PGBENCH_RES_TUPLES_OK));
	neo4j_free_result(res);
	if (!ok)
		stream->failed = true;
	return ok;
}

static PgBenchCopyStream *
neo4j_copy_begin(PgBenchConn *conn, const char *table_name,
				 int ncols, const char *const *col_names,
				 const char *copy_sql)
{
	PgBenchCopyStream *stream;

	(void) copy_sql;
	if (conn == NULL || conn->status != PGBENCH_CONN_OK || table_name == NULL)
		return NULL;

	stream = pg_malloc0_object(PgBenchCopyStream);
	stream->conn = conn;
	stream->table_name = pg_strdup(table_name);

	if (ncols > 0 && col_names != NULL)
	{
		stream->ncols = ncols;
		stream->col_names = pg_malloc0_array(char *, ncols);
		for (int i = 0; i < ncols; i++)
			stream->col_names[i] = pg_strdup(col_names[i]);
	}
	else
	{
		neo4j_populate_default_cols(stream, table_name);
	}

	initPQExpBuffer(&stream->line_buf);
	initPQExpBuffer(&stream->rows_json);
	stream->buffered_rows = 0;
	return stream;
}

static void
neo4j_append_copy_text_line(PgBenchCopyStream *stream, const char *line, size_t len)
{
	const char *p = line;
	const char *end = line + len;
	int			col_idx = 0;

	if (len > 0 && end[-1] == '\r')
		end--;
	if (p == end)
		return;

	if (stream->buffered_rows > 0)
		appendPQExpBufferChar(&stream->rows_json, ',');
	appendPQExpBufferChar(&stream->rows_json, '{');

	for (;;)
	{
		const char *tab = memchr(p, '\t', end - p);
		const char *fend = tab ? tab : end;
		size_t		flen = (size_t) (fend - p);
		char		fallback_col[32];
		const char *cname;

		if (col_idx > 0)
			appendPQExpBufferChar(&stream->rows_json, ',');

		if (col_idx < stream->ncols && stream->col_names != NULL)
		{
			cname = stream->col_names[col_idx];
		}
		else
		{
			snprintf(fallback_col, sizeof(fallback_col), "col%d", col_idx + 1);
			cname = fallback_col;
		}
		neo4j_append_json_str(&stream->rows_json, cname);
		appendPQExpBufferChar(&stream->rows_json, ':');

		if (flen == 2 && p[0] == '\\' && p[1] == 'N')
		{
			appendPQExpBufferStr(&stream->rows_json, "null");
		}
		else
		{
			char	   *tok = pnstrdup(p, flen);
			int64		ival;
			double		dval;

			if (neo4j_str_is_canonical_int(tok, &ival))
				appendPQExpBuffer(&stream->rows_json, INT64_FORMAT, ival);
			else if (neo4j_str_is_canonical_double(tok, &dval))
				appendPQExpBuffer(&stream->rows_json, "%.15g", dval);
			else
				neo4j_append_json_str(&stream->rows_json, tok);
			pg_free(tok);
		}

		col_idx++;
		if (tab == NULL)
			break;
		p = tab + 1;
	}

	appendPQExpBufferChar(&stream->rows_json, '}');
	stream->buffered_rows++;
}

static bool
neo4j_copy_put_data(PgBenchCopyStream *stream, const char *buffer, int nbytes)
{
	const char *p;
	const char *nl;

	if (stream == NULL || stream->failed || nbytes < 0)
		return false;
	if (nbytes == 0)
		return true;

	appendBinaryPQExpBuffer(&stream->line_buf, buffer, (size_t) nbytes);
	p = stream->line_buf.data;
	while ((nl = strchr(p, '\n')) != NULL)
	{
		neo4j_append_copy_text_line(stream, p, (size_t) (nl - p));
		p = nl + 1;
		if (stream->buffered_rows >= 500 || stream->rows_json.len >= 262144)
		{
			if (!neo4j_flush_copy_batch(stream))
				return false;
		}
	}

	if (p > stream->line_buf.data)
	{
		size_t		rem = strlen(p);

		memmove(stream->line_buf.data, p, rem + 1);
		stream->line_buf.len = rem;
	}
	return true;
}

static bool
neo4j_copy_put_row(PgBenchCopyStream *stream, int ncols,
				   const struct PgBenchValue *col_values)
{
	if (stream == NULL || stream->failed || col_values == NULL || ncols <= 0)
		return false;

	if (stream->buffered_rows > 0)
		appendPQExpBufferChar(&stream->rows_json, ',');
	appendPQExpBufferChar(&stream->rows_json, '{');

	for (int c = 0; c < ncols; c++)
	{
		const PgBenchValue *v = &col_values[c];
		char		fallback_col[32];
		const char *cname;

		if (c > 0)
			appendPQExpBufferChar(&stream->rows_json, ',');

		if (c < stream->ncols && stream->col_names != NULL)
		{
			cname = stream->col_names[c];
		}
		else
		{
			snprintf(fallback_col, sizeof(fallback_col), "col%d", c + 1);
			cname = fallback_col;
		}
		neo4j_append_json_str(&stream->rows_json, cname);
		appendPQExpBufferChar(&stream->rows_json, ':');
		neo4j_append_pgbench_value_json(&stream->rows_json, v);
	}

	appendPQExpBufferChar(&stream->rows_json, '}');
	stream->buffered_rows++;

	if (stream->buffered_rows >= 500 || stream->rows_json.len >= 262144)
	{
		if (!neo4j_flush_copy_batch(stream))
			return false;
	}
	return true;
}

static bool
neo4j_copy_end(PgBenchCopyStream *stream, const char *errormsg)
{
	bool		ok = true;

	if (stream == NULL)
		return false;

	if (errormsg == NULL && !stream->failed)
	{
		if (stream->line_buf.len > 0)
		{
			neo4j_append_copy_text_line(stream, stream->line_buf.data,
										stream->line_buf.len);
			resetPQExpBuffer(&stream->line_buf);
		}
		ok = neo4j_flush_copy_batch(stream);
	}
	else
	{
		ok = false;
	}

	termPQExpBuffer(&stream->line_buf);
	termPQExpBuffer(&stream->rows_json);
	pg_free(stream->table_name);
	if (stream->col_names)
	{
		for (int i = 0; i < stream->ncols; i++)
			pg_free(stream->col_names[i]);
		pg_free(stream->col_names);
	}
	pg_free(stream);
	return ok;
}

/*-------------------------------------------------------------------------
 * Exported Neo4j Driver Vtable
 *-------------------------------------------------------------------------
 */

const PgBenchDbDriver neo4j_driver = {
	.name = "neo4j",
	.caps = {
		.supports_pipeline = true,
		.supports_async_connect = false,
		.supports_vacuum = false,
		.supports_pg_catalog = false,
		.supports_copy_text = true,
		.supports_typed_copy = true,
		.prefer_prepared_mode = false,
		.param_style = PGBENCH_PARAM_DOLLAR
	},
	.init_global = neo4j_init_global,
	.shutdown_global = neo4j_shutdown_global,
	.connect_sync = neo4j_connect_sync,
	.connect_start = neo4j_connect_start,
	.connect_poll = neo4j_connect_poll,
	.finish = neo4j_finish,
	.status = neo4j_status,
	.error_message = neo4j_error_message,
	.get_poll_fd = neo4j_get_poll_fd,
	.tx_status = neo4j_tx_status,
	.get_host = neo4j_get_host,
	.get_port = neo4j_get_port,
	.get_db = neo4j_get_db,
	.set_cancel_conn = neo4j_set_cancel_conn,
	.reset_cancel_conn = neo4j_reset_cancel_conn,
	.exec_sync = neo4j_exec_sync,
	.prepare_sync = neo4j_prepare_sync,
	.escape_identifier = neo4j_escape_identifier,
	.free_mem = neo4j_free_mem,
	.send_query = neo4j_send_query,
	.send_query_params = neo4j_send_query_params,
	.send_prepare = neo4j_send_prepare,
	.send_query_prepared = neo4j_send_query_prepared,
	.consume_input = neo4j_consume_input,
	.is_busy = neo4j_is_busy,
	.get_result = neo4j_get_result,
	.enter_pipeline_mode = neo4j_enter_pipeline_mode,
	.exit_pipeline_mode = neo4j_exit_pipeline_mode,
	.pipeline_status = neo4j_pipeline_status,
	.pipeline_sync = neo4j_pipeline_sync,
	.send_pipeline_sync = neo4j_send_pipeline_sync,
	.result_status = neo4j_result_status,
	.result_error_field = neo4j_result_error_field,
	.result_error_message = neo4j_result_error_message,
	.ntuples = neo4j_ntuples,
	.nfields = neo4j_nfields,
	.fname = neo4j_fname,
	.fnumber = neo4j_fnumber,
	.get_is_null = neo4j_get_is_null,
	.get_value = neo4j_get_value,
	.clear_result = neo4j_clear_result,
	.copy_begin = neo4j_copy_begin,
	.copy_put_data = neo4j_copy_put_data,
	.copy_put_row = neo4j_copy_put_row,
	.copy_end = neo4j_copy_end
};
