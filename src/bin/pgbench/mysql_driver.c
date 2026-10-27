/*-------------------------------------------------------------------------
 *
 * mysql_driver.c
 *		MySQL / MariaDB wire-protocol database driver implementation of
 *		PgBenchDbDriver for pgbench.
 *
 * Implements the MySQL v10 client/server protocol directly over non-blocking
 * TCP sockets without requiring external libmysqlclient headers:
 *   - Connection URI parsing: mysql://[user[:pass]@]host[:port][/db] and
 *     mariadb://[user[:pass]@]host[:port][/db] (default port 3306).
 *   - Protocol v10 Initial Handshake & HandshakeResponse41 with built-in
 *     SHA-1 (mysql_native_password) authentication, AuthSwitchRequest (0xfe),
 *     and AuthMoreData (0x01).
 *   - Non-blocking socket polling integrated with pgbench's socket_set.
 *   - Text protocol (COM_QUERY 0x03) and Prepared Statement binary protocol
 *     (COM_STMT_PREPARE 0x16, COM_STMT_EXECUTE 0x17) with automatic
 *     translation of PostgreSQL $1, $2 placeholders to MySQL ? placeholders.
 *   - TCP command pipelining (\startpipeline / \endpipeline).
 *   - Automatic mapping of ER_LOCK_DEADLOCK (1213) and ER_LOCK_WAIT_TIMEOUT
 *     (1205) to SQLSTATE "40001" for pgbench deadlock/serialization retries.
 *   - Multi-row batched INSERT bulk loading for copy_begin / copy_put_data /
 *     copy_put_row / copy_end.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pgbench/mysql_driver.c
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

#include "common/logging.h"
#include "common/string.h"
#include "db_driver.h"
#include "fe_utils/string_utils.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "variable.h"

#define MYSQL_DEFAULT_PORT "3306"

/* MySQL capability flags */
#define MYSQL_CLIENT_LONG_PASSWORD		0x00000001UL
#define MYSQL_CLIENT_FOUND_ROWS			0x00000002UL
#define MYSQL_CLIENT_LONG_FLAG			0x00000004UL
#define MYSQL_CLIENT_CONNECT_WITH_DB	0x00000008UL
#define MYSQL_CLIENT_PROTOCOL_41		0x00000200UL
#define MYSQL_CLIENT_TRANSACTIONS		0x00002000UL
#define MYSQL_CLIENT_SECURE_CONNECTION	0x00008000UL
#define MYSQL_CLIENT_MULTI_STATEMENTS	0x00010000UL
#define MYSQL_CLIENT_MULTI_RESULTS		0x00020000UL
#define MYSQL_CLIENT_PS_MULTI_RESULTS	0x00040000UL
#define MYSQL_CLIENT_PLUGIN_AUTH		0x00080000UL
#define MYSQL_CLIENT_DEPRECATE_EOF		0x01000000UL

/* MySQL server status flags */
#define MYSQL_SERVER_STATUS_IN_TRANS			0x0001U
#define MYSQL_SERVER_MORE_RESULTS_EXISTS		0x0008U

/* MySQL command bytes */
#define MYSQL_COM_QUIT			0x01
#define MYSQL_COM_INIT_DB		0x02
#define MYSQL_COM_QUERY			0x03
#define MYSQL_COM_PING			0x0e
#define MYSQL_COM_STMT_PREPARE	0x16
#define MYSQL_COM_STMT_EXECUTE	0x17
#define MYSQL_COM_STMT_CLOSE	0x19
#define MYSQL_COM_STMT_RESET	0x1a

/* MySQL column/parameter field types */
#define MYSQL_TYPE_DECIMAL		0x00
#define MYSQL_TYPE_TINY			0x01
#define MYSQL_TYPE_SHORT		0x02
#define MYSQL_TYPE_LONG			0x03
#define MYSQL_TYPE_FLOAT		0x04
#define MYSQL_TYPE_DOUBLE		0x05
#define MYSQL_TYPE_NULL			0x06
#define MYSQL_TYPE_TIMESTAMP	0x07
#define MYSQL_TYPE_LONGLONG		0x08
#define MYSQL_TYPE_INT24		0x09
#define MYSQL_TYPE_DATE			0x0a
#define MYSQL_TYPE_TIME			0x0b
#define MYSQL_TYPE_DATETIME		0x0c
#define MYSQL_TYPE_YEAR			0x0d
#define MYSQL_TYPE_VARCHAR		0x0f
#define MYSQL_TYPE_BIT			0x10
#define MYSQL_TYPE_JSON			0xf5
#define MYSQL_TYPE_NEWDECIMAL	0xf6
#define MYSQL_TYPE_ENUM			0xf7
#define MYSQL_TYPE_SET			0xf8
#define MYSQL_TYPE_TINY_BLOB	0xf9
#define MYSQL_TYPE_MEDIUM_BLOB	0xfa
#define MYSQL_TYPE_LONG_BLOB	0xfb
#define MYSQL_TYPE_BLOB			0xfc
#define MYSQL_TYPE_VAR_STRING	0xfd
#define MYSQL_TYPE_STRING		0xfe
#define MYSQL_TYPE_GEOMETRY		0xff

#define MYSQL_FLAG_UNSIGNED		0x0020U

/*-------------------------------------------------------------------------
 * Minimal self-contained SHA-1 helper for MySQL authentication
 *-------------------------------------------------------------------------
 */

typedef struct MysqlSha1Ctx
{
	uint32		state[5];
	uint64		count;
	uint8		buffer[64];
} MysqlSha1Ctx;

static inline uint32
mysql_rotl32(uint32 x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void
mysql_sha1_transform(uint32 state[5], const uint8 block[64])
{
	uint32		w[80];
	uint32		a,
				b,
				c,
				d,
				e;

	for (int i = 0; i < 16; i++)
	{
		w[i] = ((uint32) block[i * 4] << 24) |
			((uint32) block[i * 4 + 1] << 16) |
			((uint32) block[i * 4 + 2] << 8) |
			((uint32) block[i * 4 + 3]);
	}
	for (int i = 16; i < 80; i++)
		w[i] = mysql_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	for (int i = 0; i < 80; i++)
	{
		uint32		f,
					k,
					temp;

		if (i < 20)
		{
			f = (b & c) | ((~b) & d);
			k = 0x5A827999U;
		}
		else if (i < 40)
		{
			f = b ^ c ^ d;
			k = 0x6ED9EBA1U;
		}
		else if (i < 60)
		{
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCU;
		}
		else
		{
			f = b ^ c ^ d;
			k = 0xCA62C1D6U;
		}

		temp = mysql_rotl32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = mysql_rotl32(b, 30);
		b = a;
		a = temp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

static void
mysql_sha1_init(MysqlSha1Ctx *ctx)
{
	ctx->state[0] = 0x67452301U;
	ctx->state[1] = 0xEFCDAB89U;
	ctx->state[2] = 0x98BADCFEU;
	ctx->state[3] = 0x10325476U;
	ctx->state[4] = 0xC3D2E1F0U;
	ctx->count = 0;
}

static void
mysql_sha1_update(MysqlSha1Ctx *ctx, const uint8 *data, size_t len)
{
	size_t		idx = (size_t) (ctx->count & 0x3F);

	ctx->count += len;
	while (len > 0)
	{
		size_t		take = 64 - idx;

		if (take > len)
			take = len;
		memcpy(ctx->buffer + idx, data, take);
		idx += take;
		data += take;
		len -= take;
		if (idx == 64)
		{
			mysql_sha1_transform(ctx->state, ctx->buffer);
			idx = 0;
		}
	}
}

static void
mysql_sha1_final(MysqlSha1Ctx *ctx, uint8 digest[20])
{
	uint64		total_bits = ctx->count * 8;
	uint8		pad = 0x80;
	uint8		zero = 0x00;
	uint8		len_bytes[8];

	mysql_sha1_update(ctx, &pad, 1);
	while ((ctx->count & 0x3F) != 56)
		mysql_sha1_update(ctx, &zero, 1);

	for (int i = 0; i < 8; i++)
		len_bytes[7 - i] = (uint8) ((total_bits >> (i * 8)) & 0xFF);
	mysql_sha1_update(ctx, len_bytes, 8);

	for (int i = 0; i < 5; i++)
	{
		digest[i * 4] = (uint8) ((ctx->state[i] >> 24) & 0xFF);
		digest[i * 4 + 1] = (uint8) ((ctx->state[i] >> 16) & 0xFF);
		digest[i * 4 + 2] = (uint8) ((ctx->state[i] >> 8) & 0xFF);
		digest[i * 4 + 3] = (uint8) (ctx->state[i] & 0xFF);
	}
}

/*
 * Compute mysql_native_password 20-byte token:
 *   SHA1(password) XOR SHA1(scramble[0..19] || SHA1(SHA1(password)))
 */
static void
mysql_native_password_scramble(const char *password, const uint8 scramble[20],
							   uint8 out_token[20])
{
	MysqlSha1Ctx ctx;
	uint8		stage1[20];
	uint8		stage2[20];
	uint8		stage3[20];

	mysql_sha1_init(&ctx);
	mysql_sha1_update(&ctx, (const uint8 *) password, strlen(password));
	mysql_sha1_final(&ctx, stage1);

	mysql_sha1_init(&ctx);
	mysql_sha1_update(&ctx, stage1, 20);
	mysql_sha1_final(&ctx, stage2);

	mysql_sha1_init(&ctx);
	mysql_sha1_update(&ctx, scramble, 20);
	mysql_sha1_update(&ctx, stage2, 20);
	mysql_sha1_final(&ctx, stage3);

	for (int i = 0; i < 20; i++)
		out_token[i] = stage1[i] ^ stage3[i];
}

/*-------------------------------------------------------------------------
 * Minimal self-contained SHA-256 helper for caching_sha2_password auth
 *-------------------------------------------------------------------------
 */

typedef struct MysqlSha256Ctx
{
	uint32		state[8];
	uint64		count;
	uint8		buffer[64];
} MysqlSha256Ctx;

static inline uint32
mysql_rotr32(uint32 x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static const uint32 mysql_sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void
mysql_sha256_transform(uint32 state[8], const uint8 block[64])
{
	uint32		w[64];
	uint32		a,
				b,
				c,
				d,
				e,
				f,
				g,
				h;

	for (int i = 0; i < 16; i++)
	{
		w[i] = ((uint32) block[i * 4] << 24) |
			((uint32) block[i * 4 + 1] << 16) |
			((uint32) block[i * 4 + 2] << 8) |
			((uint32) block[i * 4 + 3]);
	}
	for (int i = 16; i < 64; i++)
	{
		uint32		s0 = mysql_rotr32(w[i - 15], 7) ^ mysql_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32		s1 = mysql_rotr32(w[i - 2], 17) ^ mysql_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	for (int i = 0; i < 64; i++)
	{
		uint32		S1 = mysql_rotr32(e, 6) ^ mysql_rotr32(e, 11) ^ mysql_rotr32(e, 25);
		uint32		ch = (e & f) ^ ((~e) & g);
		uint32		temp1 = h + S1 + ch + mysql_sha256_k[i] + w[i];
		uint32		S0 = mysql_rotr32(a, 2) ^ mysql_rotr32(a, 13) ^ mysql_rotr32(a, 22);
		uint32		maj = (a & b) ^ (a & c) ^ (b & c);
		uint32		temp2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

static void
mysql_sha256_init(MysqlSha256Ctx *ctx)
{
	ctx->state[0] = 0x6a09e667U;
	ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U;
	ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU;
	ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU;
	ctx->state[7] = 0x5be0cd19U;
	ctx->count = 0;
}

static void
mysql_sha256_update(MysqlSha256Ctx *ctx, const uint8 *data, size_t len)
{
	size_t		idx = (size_t) (ctx->count & 0x3F);

	ctx->count += len;
	while (len > 0)
	{
		size_t		take = 64 - idx;

		if (take > len)
			take = len;
		memcpy(ctx->buffer + idx, data, take);
		idx += take;
		data += take;
		len -= take;
		if (idx == 64)
		{
			mysql_sha256_transform(ctx->state, ctx->buffer);
			idx = 0;
		}
	}
}

static void
mysql_sha256_final(MysqlSha256Ctx *ctx, uint8 digest[32])
{
	uint64		total_bits = ctx->count * 8;
	uint8		pad = 0x80;
	uint8		zero = 0x00;
	uint8		len_bytes[8];

	mysql_sha256_update(ctx, &pad, 1);
	while ((ctx->count & 0x3F) != 56)
		mysql_sha256_update(ctx, &zero, 1);

	for (int i = 0; i < 8; i++)
		len_bytes[7 - i] = (uint8) ((total_bits >> (i * 8)) & 0xFF);
	mysql_sha256_update(ctx, len_bytes, 8);

	for (int i = 0; i < 8; i++)
	{
		digest[i * 4] = (uint8) ((ctx->state[i] >> 24) & 0xFF);
		digest[i * 4 + 1] = (uint8) ((ctx->state[i] >> 16) & 0xFF);
		digest[i * 4 + 2] = (uint8) ((ctx->state[i] >> 8) & 0xFF);
		digest[i * 4 + 3] = (uint8) (ctx->state[i] & 0xFF);
	}
}

/*
 * Compute caching_sha2_password 32-byte token:
 *   XOR(SHA256(password), SHA256(SHA256(SHA256(password)) || scramble[0..19]))
 */
static void
mysql_caching_sha2_password_scramble(const char *password, const uint8 scramble[20],
									 uint8 out_token[32])
{
	MysqlSha256Ctx ctx;
	uint8		stage1[32];
	uint8		stage2[32];
	uint8		stage3[32];

	mysql_sha256_init(&ctx);
	mysql_sha256_update(&ctx, (const uint8 *) password, strlen(password));
	mysql_sha256_final(&ctx, stage1);

	mysql_sha256_init(&ctx);
	mysql_sha256_update(&ctx, stage1, 32);
	mysql_sha256_final(&ctx, stage2);

	mysql_sha256_init(&ctx);
	mysql_sha256_update(&ctx, stage2, 32);
	mysql_sha256_update(&ctx, scramble, 20);
	mysql_sha256_final(&ctx, stage3);

	for (int i = 0; i < 32; i++)
		out_token[i] = stage1[i] ^ stage3[i];
}

/*-------------------------------------------------------------------------
 * Driver data structures
 *-------------------------------------------------------------------------
 */

typedef enum MysqlHandshakeState
{
	MYSQL_HS_NONE = 0,
	MYSQL_HS_TCP_CONNECTING,
	MYSQL_HS_READ_GREETING,
	MYSQL_HS_WRITE_AUTH,
	MYSQL_HS_READ_AUTH_RESULT,
	MYSQL_HS_DONE,
	MYSQL_HS_FAILED
} MysqlHandshakeState;

typedef enum MysqlPendingType
{
	MYSQL_PENDING_QUERY = 0,
	MYSQL_PENDING_PREPARE,
	MYSQL_PENDING_STMT_EXECUTE,
	MYSQL_PENDING_SYNC,
	MYSQL_PENDING_LOCAL_OK
} MysqlPendingType;

typedef enum MysqlReadPhase
{
	MYSQL_PHASE_FIRST_PKT = 0,
	MYSQL_PHASE_PREPARE_PARAMS,
	MYSQL_PHASE_PREPARE_COLS,
	MYSQL_PHASE_COLS,
	MYSQL_PHASE_ROWS
} MysqlReadPhase;

typedef struct MysqlPreparedStmt
{
	char	   *name;
	char	   *translated_sql;
	uint32		stmt_id;
	int			num_params;
	int			num_columns;
	int		   *param_map;		/* 0-based $n index for each ? placeholder */
	int			nmap;
	bool		client_emulated;
} MysqlPreparedStmt;

typedef struct MysqlPendingCmd
{
	MysqlPendingType type;
	char	   *stmt_name;
	char	   *translated_sql;
	int		   *param_map;
	int			nmap;
	struct MysqlPendingCmd *next;
} MysqlPendingCmd;

struct PgBenchResult
{
	PgBenchExecStatus status;
	char	   *err_msg;
	char		sqlstate[6];
	int			nrows;
	int			ncols;
	char	  **col_names;
	uint8	   *col_types;
	uint16	   *col_flags;
	char	 ***cells;
	bool	  **is_null;
	int			row_cap;
	struct PgBenchResult *next;
};

struct PgBenchCopyStream
{
	PgBenchConn *conn;
	char	   *table_name;
	char	  **col_names;
	int			ncols;
	PQExpBufferData line_buf;
	PQExpBufferData sql_buf;
	int			buffered_rows;
	bool		failed;
};

struct PgBenchConn
{
	int			sock;
	PgBenchConnStatus status;
	PgBenchTxStatus tx_status;
	PgBenchPipelineStatus pipe_status;
	bool		pipeline_aborted;

	char	   *host;
	char	   *port;
	char	   *dbname;
	char	   *user;
	char	   *password;
	PQExpBufferData err_buf;

	/* Handshake state */
	MysqlHandshakeState hs_state;
	uint32		server_caps;
	uint8		scramble[20];
	char		auth_plugin[64];
	uint8		hs_seq_id;

	/* Non-blocking write buffer */
	char	   *out_buf;
	size_t		out_len;
	size_t		out_sent;
	size_t		out_cap;

	/* Non-blocking read buffer */
	uint8	   *in_buf;
	size_t		in_len;
	size_t		in_parsed;
	size_t		in_cap;
	PQExpBufferData multipkt_buf;
	bool		in_multipkt;

	/* Pending command FIFO queue */
	MysqlPendingCmd *pending_head;
	MysqlPendingCmd *pending_tail;

	/* State for the currently-decoding command response */
	MysqlReadPhase read_phase;
	uint32		prep_stmt_id;
	int			prep_num_params;
	int			prep_num_cols;
	int			prep_params_rem;
	int			prep_cols_rem;
	int			cols_expected;
	int			cols_read;
	PgBenchResult *building_res;

	/* Completed results FIFO queue */
	PgBenchResult *res_head;
	PgBenchResult *res_tail;
	bool		result_drained;

	/* Prepared statement registry */
	MysqlPreparedStmt *prep_stmts;
	int			nprep_stmts;
	int			prep_cap;
};

static char *saved_mysql_conninfo = NULL;

/*-------------------------------------------------------------------------
 * Buffer & Packet Helpers
 *-------------------------------------------------------------------------
 */

static inline uint16
mysql_read_u16_le(const uint8 *p)
{
	return (uint16) p[0] | ((uint16) p[1] << 8);
}

static inline uint32
mysql_read_u24_le(const uint8 *p)
{
	return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16);
}

static inline uint32
mysql_read_u32_le(const uint8 *p)
{
	return (uint32) p[0] |
		((uint32) p[1] << 8) |
		((uint32) p[2] << 16) |
		((uint32) p[3] << 24);
}

static inline uint64
mysql_read_u64_le(const uint8 *p)
{
	return (uint64) mysql_read_u32_le(p) |
		((uint64) mysql_read_u32_le(p + 4) << 32);
}

static inline void
mysql_write_u16_le(uint8 *p, uint16 v)
{
	p[0] = (uint8) (v & 0xFF);
	p[1] = (uint8) ((v >> 8) & 0xFF);
}

static inline void
mysql_write_u32_le(uint8 *p, uint32 v)
{
	p[0] = (uint8) (v & 0xFF);
	p[1] = (uint8) ((v >> 8) & 0xFF);
	p[2] = (uint8) ((v >> 16) & 0xFF);
	p[3] = (uint8) ((v >> 24) & 0xFF);
}

static inline void
mysql_write_u64_le(uint8 *p, uint64 v)
{
	mysql_write_u32_le(p, (uint32) (v & 0xFFFFFFFFULL));
	mysql_write_u32_le(p + 4, (uint32) (v >> 32));
}

/*
 * Read a MySQL length-encoded integer from [p, end).
 * Sets *is_null = true if byte is 0xFB.
 */
static bool
mysql_read_lenenc_int(const uint8 **pp, const uint8 *end, uint64 *val_out, bool *is_null)
{
	const uint8 *p = *pp;
	uint8		first;

	if (is_null)
		*is_null = false;
	if (p >= end)
		return false;

	first = *p++;
	if (first < 0xFB)
	{
		*val_out = first;
		*pp = p;
		return true;
	}
	else if (first == 0xFB)
	{
		if (is_null)
			*is_null = true;
		*val_out = 0;
		*pp = p;
		return true;
	}
	else if (first == 0xFC)
	{
		if (end - p < 2)
			return false;
		*val_out = mysql_read_u16_le(p);
		*pp = p + 2;
		return true;
	}
	else if (first == 0xFD)
	{
		if (end - p < 3)
			return false;
		*val_out = mysql_read_u24_le(p);
		*pp = p + 3;
		return true;
	}
	else if (first == 0xFE)
	{
		if (end - p < 8)
			return false;
		*val_out = mysql_read_u64_le(p);
		*pp = p + 8;
		return true;
	}

	return false;
}

static void
mysql_append_lenenc_int(PQExpBuffer buf, uint64 val)
{
	if (val < 251)
	{
		appendPQExpBufferChar(buf, (char) (uint8) val);
	}
	else if (val < 65536ULL)
	{
		uint8		b[3];

		b[0] = 0xFC;
		mysql_write_u16_le(b + 1, (uint16) val);
		appendBinaryPQExpBuffer(buf, (const char *) b, 3);
	}
	else if (val < 16777216ULL)
	{
		uint8		b[4];

		b[0] = 0xFD;
		b[1] = (uint8) (val & 0xFF);
		b[2] = (uint8) ((val >> 8) & 0xFF);
		b[3] = (uint8) ((val >> 16) & 0xFF);
		appendBinaryPQExpBuffer(buf, (const char *) b, 4);
	}
	else
	{
		uint8		b[9];

		b[0] = 0xFE;
		mysql_write_u64_le(b + 1, val);
		appendBinaryPQExpBuffer(buf, (const char *) b, 9);
	}
}

static void
mysql_append_lenenc_str(PQExpBuffer buf, const char *str, size_t len)
{
	mysql_append_lenenc_int(buf, (uint64) len);
	if (len > 0)
		appendBinaryPQExpBuffer(buf, str, len);
}

static void
mysql_queue_bytes(PgBenchConn *conn, const void *data, size_t len)
{
	if (conn->out_sent == conn->out_len)
	{
		conn->out_sent = 0;
		conn->out_len = 0;
	}
	else if (conn->out_sent > 65536 && conn->out_sent * 2 >= conn->out_len)
	{
		size_t		rem = conn->out_len - conn->out_sent;

		memmove(conn->out_buf, conn->out_buf + conn->out_sent, rem);
		conn->out_len = rem;
		conn->out_sent = 0;
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

static void
mysql_queue_packet(PgBenchConn *conn, uint8 seq_id, const void *payload, size_t len)
{
	const uint8 *p = (const uint8 *) payload;
	size_t		rem = len;

	do
	{
		size_t		chunk = (rem > 0xFFFFFFU) ? 0xFFFFFFU : rem;
		uint8		hdr[4];

		hdr[0] = (uint8) (chunk & 0xFF);
		hdr[1] = (uint8) ((chunk >> 8) & 0xFF);
		hdr[2] = (uint8) ((chunk >> 16) & 0xFF);
		hdr[3] = seq_id++;
		mysql_queue_bytes(conn, hdr, 4);
		if (chunk > 0)
		{
			mysql_queue_bytes(conn, p, chunk);
			p += chunk;
			rem -= chunk;
		}
		if (chunk == 0xFFFFFFU && rem == 0)
		{
			hdr[0] = 0;
			hdr[1] = 0;
			hdr[2] = 0;
			hdr[3] = seq_id++;
			mysql_queue_bytes(conn, hdr, 4);
		}
	} while (rem > 0);
}

static bool
mysql_flush_out(PgBenchConn *conn)
{
	while (conn->out_sent < conn->out_len)
	{
		ssize_t		n = send(conn->sock,
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
							  "MySQL socket send failed: %s\n",
							  strerror(errno));
			conn->status = PGBENCH_CONN_BAD;
			return false;
		}
	}

	conn->out_sent = 0;
	conn->out_len = 0;
	return true;
}

/*-------------------------------------------------------------------------
 * Result & Command Queue Management
 *-------------------------------------------------------------------------
 */

static PgBenchResult *
mysql_alloc_result(PgBenchExecStatus status)
{
	PgBenchResult *res = pg_malloc0_object(PgBenchResult);

	res->status = status;
	return res;
}

static void
mysql_free_result(PgBenchResult *res)
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
	pg_free(res->col_types);
	pg_free(res->col_flags);
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
mysql_push_result(PgBenchConn *conn, PgBenchResult *res)
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
mysql_push_pending(PgBenchConn *conn, MysqlPendingType type,
				   const char *stmt_name, const char *translated_sql,
				   const int *param_map, int nmap)
{
	MysqlPendingCmd *cmd = pg_malloc0_object(MysqlPendingCmd);

	cmd->type = type;
	if (stmt_name)
		cmd->stmt_name = pg_strdup(stmt_name);
	if (translated_sql)
		cmd->translated_sql = pg_strdup(translated_sql);
	if (param_map && nmap > 0)
	{
		cmd->param_map = pg_malloc_array(int, nmap);
		memcpy(cmd->param_map, param_map, sizeof(int) * nmap);
		cmd->nmap = nmap;
	}
	if (conn->pending_tail)
		conn->pending_tail->next = cmd;
	else
		conn->pending_head = cmd;
	conn->pending_tail = cmd;
	conn->result_drained = false;
}

static void
mysql_pop_pending(PgBenchConn *conn)
{
	MysqlPendingCmd *cmd = conn->pending_head;

	if (cmd == NULL)
		return;
	conn->pending_head = cmd->next;
	if (conn->pending_head == NULL)
		conn->pending_tail = NULL;
	pg_free(cmd->stmt_name);
	pg_free(cmd->translated_sql);
	pg_free(cmd->param_map);
	pg_free(cmd);
	conn->read_phase = MYSQL_PHASE_FIRST_PKT;
	if (conn->building_res)
	{
		mysql_free_result(conn->building_res);
		conn->building_res = NULL;
	}
}

static MysqlPreparedStmt *
mysql_find_prepared(PgBenchConn *conn, const char *name)
{
	const char *lookup = name ? name : "";

	for (int i = 0; i < conn->nprep_stmts; i++)
	{
		if (strcmp(conn->prep_stmts[i].name, lookup) == 0)
			return &conn->prep_stmts[i];
	}
	return NULL;
}

static MysqlPreparedStmt *
mysql_store_prepared(PgBenchConn *conn, const char *name,
					 const char *translated_sql, uint32 stmt_id,
					 int num_params, int num_columns,
					 const int *param_map, int nmap, bool client_emulated)
{
	MysqlPreparedStmt *entry = mysql_find_prepared(conn, name);

	if (entry == NULL)
	{
		if (conn->nprep_stmts >= conn->prep_cap)
		{
			conn->prep_cap = conn->prep_cap ? conn->prep_cap * 2 : 16;
			conn->prep_stmts = pg_realloc_array(conn->prep_stmts,
												MysqlPreparedStmt,
												conn->prep_cap);
		}
		entry = &conn->prep_stmts[conn->nprep_stmts++];
		memset(entry, 0, sizeof(*entry));
		entry->name = pg_strdup(name ? name : "");
	}
	else
	{
		pg_free(entry->translated_sql);
		pg_free(entry->param_map);
		entry->param_map = NULL;
		entry->nmap = 0;
	}

	entry->translated_sql = pg_strdup(translated_sql ? translated_sql : "");
	entry->stmt_id = stmt_id;
	entry->num_params = num_params;
	entry->num_columns = num_columns;
	entry->client_emulated = client_emulated;
	if (param_map && nmap > 0)
	{
		entry->param_map = pg_malloc_array(int, nmap);
		memcpy(entry->param_map, param_map, sizeof(int) * nmap);
		entry->nmap = nmap;
	}
	return entry;
}

/*-------------------------------------------------------------------------
 * SQL Dialect & Parameter Translation ($1 -> ?)
 *-------------------------------------------------------------------------
 */

static char *
mysql_adapt_sql(const char *sql, bool *is_noop_out)
{
	const char *p = sql;
	size_t		len;

	*is_noop_out = false;
	if (sql == NULL)
		return pg_strdup("");

	while (*p && isspace((unsigned char) *p))
		p++;

	len = strlen(p);
	while (len > 0 && (isspace((unsigned char) p[len - 1]) || p[len - 1] == ';'))
		len--;

	if ((len == 6 && pg_strncasecmp(p, "vacuum", 6) == 0) ||
		(len > 6 && pg_strncasecmp(p, "vacuum", 6) == 0 && isspace((unsigned char) p[6])) ||
		(len == 7 && pg_strncasecmp(p, "analyze", 7) == 0) ||
		(len > 7 && pg_strncasecmp(p, "analyze", 7) == 0 && isspace((unsigned char) p[7])))
	{
		*is_noop_out = true;
		return pg_strdup("");
	}

	/* PostgreSQL 'END' transaction alias -> 'COMMIT' in MySQL */
	if (len == 3 && pg_strncasecmp(p, "end", 3) == 0)
		return pg_strdup("COMMIT");

	/*
	 * Strip trailing WITH (fillfactor=...) on CREATE TABLE / ALTER TABLE
	 * so standard pgbench -i DDL works directly against MySQL.
	 */
	if (pg_strncasecmp(p, "create table", 12) == 0 ||
		pg_strncasecmp(p, "alter table", 11) == 0)
	{
		const char *with_pos = NULL;

		for (size_t i = 0; i + 5 <= len; i++)
		{
			if ((i == 0 || isspace((unsigned char) p[i - 1]) || p[i - 1] == ')') &&
				pg_strncasecmp(p + i, "with", 4) == 0 &&
				(isspace((unsigned char) p[i + 4]) || p[i + 4] == '('))
			{
				with_pos = p + i;
			}
		}
		if (with_pos != NULL)
			return pnstrdup(p, with_pos - p);
	}

	/*
	 * Multi-table TRUNCATE [TABLE] t1, t2 -> DELETE FROM t1; DELETE FROM t2
	 */
	if (pg_strncasecmp(p, "truncate", 8) == 0 && isspace((unsigned char) p[8]))
	{
		const char *q = p + 8;

		while (*q && isspace((unsigned char) *q))
			q++;
		if (pg_strncasecmp(q, "table", 5) == 0 && isspace((unsigned char) q[5]))
		{
			q += 5;
			while (*q && isspace((unsigned char) *q))
				q++;
		}
		if (strchr(q, ',') != NULL)
		{
			PQExpBufferData buf;
			const char *cur = q;
			const char *end = p + len;
			bool		first = true;

			initPQExpBuffer(&buf);
			while (cur < end)
			{
				const char *comma = memchr(cur, ',', end - cur);
				const char *seg_end = comma ? comma : end;
				const char *ts = cur;
				const char *te = seg_end;

				while (ts < te && isspace((unsigned char) *ts))
					ts++;
				while (te > ts && isspace((unsigned char) te[-1]))
					te--;
				if (te > ts)
				{
					if (!first)
						appendPQExpBufferStr(&buf, "; ");
					appendPQExpBuffer(&buf, "DELETE FROM %.*s", (int) (te - ts), ts);
					first = false;
				}
				if (!comma)
					break;
				cur = comma + 1;
			}
			return buf.data;
		}
	}

	return pg_strdup(sql);
}

/*
 * Translate PostgreSQL $1, $2, ... placeholders into MySQL ? placeholders
 * outside string literals, identifiers, and comments.
 */
static char *
mysql_translate_dollar_params(const char *sql, int **param_map_out, int *nmap_out)
{
	PQExpBufferData out;
	int		   *map = NULL;
	int			nmap = 0;
	int			cap = 0;
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
		else if (p[0] == '-' && p[1] == '-')
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
			int			idx = 0;

			p++;
			while (isdigit((unsigned char) *p))
			{
				idx = idx * 10 + (*p - '0');
				p++;
			}
			appendPQExpBufferChar(&out, '?');
			if (nmap >= cap)
			{
				cap = cap ? cap * 2 : 16;
				map = pg_realloc_array(map, int, cap);
			}
			map[nmap++] = (idx > 0) ? (idx - 1) : 0;
		}
		else if (*p == '?')
		{
			appendPQExpBufferChar(&out, *p++);
			if (nmap >= cap)
			{
				cap = cap ? cap * 2 : 16;
				map = pg_realloc_array(map, int, cap);
			}
			map[nmap] = nmap;
			nmap++;
		}
		else
		{
			appendPQExpBufferChar(&out, *p++);
		}
	}

	if (param_map_out)
		*param_map_out = map;
	else
		pg_free(map);
	if (nmap_out)
		*nmap_out = nmap;
	return out.data;
}

static void
mysql_append_escaped_literal(PQExpBuffer buf, const char *str)
{
	if (str == NULL)
	{
		appendPQExpBufferStr(buf, "NULL");
		return;
	}
	appendPQExpBufferChar(buf, '\'');
	for (const char *p = str; *p; p++)
	{
		switch (*p)
		{
			case '\'':
				appendPQExpBufferStr(buf, "''");
				break;
			case '\\':
				appendPQExpBufferStr(buf, "\\\\");
				break;
			case '\0':
				appendPQExpBufferStr(buf, "\\0");
				break;
			case '\n':
				appendPQExpBufferStr(buf, "\\n");
				break;
			case '\r':
				appendPQExpBufferStr(buf, "\\r");
				break;
			default:
				appendPQExpBufferChar(buf, *p);
				break;
		}
	}
	appendPQExpBufferChar(buf, '\'');
}

static void
mysql_append_param_literal(PQExpBuffer buf, int idx, int nparams,
						   const char *const *param_texts,
						   const struct Variable *const *param_vars)
{
	if (idx < 0 || idx >= nparams)
	{
		appendPQExpBufferStr(buf, "NULL");
		return;
	}

	if (param_vars != NULL && param_vars[idx] != NULL)
	{
		const PgBenchValue *v = &param_vars[idx]->value;

		switch (v->type)
		{
			case PGBT_NULL:
				appendPQExpBufferStr(buf, "NULL");
				return;
			case PGBT_INT:
				appendPQExpBuffer(buf, INT64_FORMAT, v->u.ival);
				return;
			case PGBT_DOUBLE:
				appendPQExpBuffer(buf, "%.15g", v->u.dval);
				return;
			case PGBT_BOOLEAN:
				appendPQExpBufferStr(buf, v->u.bval ? "1" : "0");
				return;
			case PGBT_TEXT:
				mysql_append_escaped_literal(buf, v->u.sval ? v->u.sval : "");
				return;
			case PGBT_BYTEA:
				{
					static const char hex[] = "0123456789abcdef";

					appendPQExpBufferStr(buf, "X'");
					for (int b = 0; b < v->u.byval.len; b++)
					{
						uint8		c = v->u.byval.data[b];

						appendPQExpBufferChar(buf, hex[(c >> 4) & 0x0F]);
						appendPQExpBufferChar(buf, hex[c & 0x0F]);
					}
					appendPQExpBufferChar(buf, '\'');
					return;
				}
			default:
				break;
		}
	}

	if (param_texts != NULL && param_texts[idx] != NULL)
		mysql_append_escaped_literal(buf, param_texts[idx]);
	else
		appendPQExpBufferStr(buf, "NULL");
}

static char *
mysql_bind_qmark_literals(const char *translated_sql, const int *param_map, int nmap,
						  int nparams, const char *const *param_texts,
						  const struct Variable *const *param_vars)
{
	PQExpBufferData out;
	const char *p = translated_sql;
	int			q_idx = 0;

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
		else if (*p == '?')
		{
			int			src_idx = (param_map && q_idx < nmap) ? param_map[q_idx] : q_idx;

			p++;
			q_idx++;
			mysql_append_param_literal(&out, src_idx, nparams, param_texts, param_vars);
		}
		else
		{
			appendPQExpBufferChar(&out, *p++);
		}
	}
	return out.data;
}

/*-------------------------------------------------------------------------
 * Connection URI & Parameter Parsing
 *-------------------------------------------------------------------------
 */

static void
mysql_parse_uri(const char *uri, char **host_out, char **port_out,
				char **user_out, char **pass_out, char **db_out)
{
	const char *p = NULL;
	const char *at;
	const char *slash;
	const char *qmark;
	const char *host_start;
	const char *host_end;

	if (pg_strncasecmp(uri, "mysql://", 8) == 0)
		p = uri + 8;
	else if (pg_strncasecmp(uri, "mariadb://", 10) == 0)
		p = uri + 10;
	else
		return;

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
 * Handshake Packet Processing
 *-------------------------------------------------------------------------
 */

static void
mysql_build_auth_token(const char *plugin_name, const char *password,
					   const uint8 scramble[20], PQExpBuffer token_buf)
{
	resetPQExpBuffer(token_buf);
	if (password == NULL || password[0] == '\0')
		return;

	if (plugin_name != NULL && strcmp(plugin_name, "mysql_clear_password") == 0)
	{
		appendBinaryPQExpBuffer(token_buf, password, strlen(password) + 1);
	}
	else if (plugin_name != NULL && strcmp(plugin_name, "caching_sha2_password") == 0)
	{
		uint8		token[32];

		mysql_caching_sha2_password_scramble(password, scramble, token);
		appendBinaryPQExpBuffer(token_buf, (const char *) token, 32);
	}
	else
	{
		uint8		token[20];

		mysql_native_password_scramble(password, scramble, token);
		appendBinaryPQExpBuffer(token_buf, (const char *) token, 20);
	}
}

static bool
mysql_handle_greeting_packet(PgBenchConn *conn, const uint8 *pkt, size_t pkt_len, uint8 seq_id)
{
	const uint8 *p = pkt;
	const uint8 *end = pkt + pkt_len;
	const uint8 *ver_end;
	uint16		cap_low = 0;
	uint16		cap_high = 0;
	uint8		auth_len = 0;
	uint32		client_caps;
	PQExpBufferData resp;
	PQExpBufferData auth_token;
	uint8		fixed_hdr[32];

	if (pkt_len < 1)
	{
		printfPQExpBuffer(&conn->err_buf, "MySQL handshake greeting packet is empty\n");
		return false;
	}

	if (pkt[0] == 0xFF)
	{
		const char *msg = (pkt_len > 3) ? (const char *) (pkt + 3) : "MySQL connection rejected";
		size_t		mlen = (pkt_len > 3) ? (pkt_len - 3) : strlen(msg);

		if (mlen >= 6 && msg[0] == '#')
		{
			msg += 6;
			mlen -= 6;
		}
		printfPQExpBuffer(&conn->err_buf, "MySQL handshake error: %.*s\n", (int) mlen, msg);
		return false;
	}

	if (pkt[0] != 0x0A)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "unsupported MySQL protocol version 0x%02x (expected v10 = 0x0a)\n",
						  pkt[0]);
		return false;
	}
	p++;

	ver_end = memchr(p, '\0', end - p);
	if (ver_end == NULL)
	{
		printfPQExpBuffer(&conn->err_buf, "malformed MySQL handshake greeting (version string)\n");
		return false;
	}
	p = ver_end + 1;

	if (end - p < 13)
	{
		printfPQExpBuffer(&conn->err_buf, "malformed MySQL handshake greeting (scramble part 1)\n");
		return false;
	}
	p += 4;						/* skip connection_id */
	memcpy(conn->scramble, p, 8);
	p += 8;
	p += 1;						/* skip filler */

	if (end - p >= 2)
	{
		cap_low = mysql_read_u16_le(p);
		p += 2;
	}
	if (end - p >= 1 + 2 + 2 + 1 + 10)
	{
		p += 1;					/* character_set */
		p += 2;					/* status_flags */
		cap_high = mysql_read_u16_le(p);
		p += 2;
		auth_len = *p++;
		p += 10;				/* reserved */
	}

	conn->server_caps = (uint32) cap_low | ((uint32) cap_high << 16);

	if (conn->server_caps & MYSQL_CLIENT_SECURE_CONNECTION)
	{
		size_t		part2_len = 12;

		if (auth_len > 8)
			part2_len = (size_t) (auth_len - 8);
		if (part2_len < 13)
			part2_len = 13;
		if ((size_t) (end - p) >= 12)
		{
			memcpy(conn->scramble + 8, p, 12);
			if ((size_t) (end - p) >= part2_len)
				p += part2_len;
			else
				p = end;
		}
	}

	snprintf(conn->auth_plugin, sizeof(conn->auth_plugin), "mysql_native_password");
	if ((conn->server_caps & MYSQL_CLIENT_PLUGIN_AUTH) && p < end)
	{
		const uint8 *plug_end = memchr(p, '\0', end - p);
		size_t		plen = plug_end ? (size_t) (plug_end - p) : (size_t) (end - p);

		if (plen > 0 && plen < sizeof(conn->auth_plugin))
		{
			memcpy(conn->auth_plugin, p, plen);
			conn->auth_plugin[plen] = '\0';
		}
	}

	client_caps = MYSQL_CLIENT_LONG_PASSWORD |
		MYSQL_CLIENT_FOUND_ROWS |
		MYSQL_CLIENT_LONG_FLAG |
		MYSQL_CLIENT_PROTOCOL_41 |
		MYSQL_CLIENT_TRANSACTIONS |
		MYSQL_CLIENT_SECURE_CONNECTION |
		MYSQL_CLIENT_MULTI_STATEMENTS |
		MYSQL_CLIENT_MULTI_RESULTS |
		MYSQL_CLIENT_PS_MULTI_RESULTS |
		MYSQL_CLIENT_PLUGIN_AUTH;

	if (conn->dbname != NULL && conn->dbname[0] != '\0')
		client_caps |= MYSQL_CLIENT_CONNECT_WITH_DB;

	memset(fixed_hdr, 0, sizeof(fixed_hdr));
	mysql_write_u32_le(fixed_hdr, client_caps);
	mysql_write_u32_le(fixed_hdr + 4, 0x01000000U); /* max_packet_size = 16MB */
	fixed_hdr[8] = 33;			/* utf8_general_ci */

	initPQExpBuffer(&resp);
	initPQExpBuffer(&auth_token);

	appendBinaryPQExpBuffer(&resp, (const char *) fixed_hdr, 32);
	appendBinaryPQExpBuffer(&resp, conn->user, strlen(conn->user) + 1);

	mysql_build_auth_token(conn->auth_plugin, conn->password, conn->scramble, &auth_token);
	appendPQExpBufferChar(&resp, (char) (uint8) auth_token.len);
	if (auth_token.len > 0)
		appendBinaryPQExpBuffer(&resp, auth_token.data, auth_token.len);

	if (client_caps & MYSQL_CLIENT_CONNECT_WITH_DB)
		appendBinaryPQExpBuffer(&resp, conn->dbname, strlen(conn->dbname) + 1);

	appendBinaryPQExpBuffer(&resp, conn->auth_plugin, strlen(conn->auth_plugin) + 1);

	conn->hs_seq_id = seq_id + 1;
	mysql_queue_packet(conn, conn->hs_seq_id, resp.data, resp.len);

	termPQExpBuffer(&auth_token);
	termPQExpBuffer(&resp);

	conn->hs_state = MYSQL_HS_WRITE_AUTH;
	return true;
}

static bool
mysql_handle_auth_result_packet(PgBenchConn *conn, const uint8 *pkt, size_t pkt_len, uint8 seq_id)
{
	if (pkt_len < 1)
	{
		printfPQExpBuffer(&conn->err_buf, "empty MySQL auth result packet\n");
		return false;
	}

	if (pkt[0] == 0x00)
	{
		conn->hs_state = MYSQL_HS_DONE;
		conn->status = PGBENCH_CONN_OK;
		return true;
	}

	if (pkt[0] == 0xFF)
	{
		const char *msg = (pkt_len > 3) ? (const char *) (pkt + 3) : "MySQL authentication failed";
		size_t		mlen = (pkt_len > 3) ? (pkt_len - 3) : strlen(msg);

		if (mlen >= 6 && msg[0] == '#')
		{
			msg += 6;
			mlen -= 6;
		}
		printfPQExpBuffer(&conn->err_buf, "MySQL authentication error: %.*s\n", (int) mlen, msg);
		conn->hs_state = MYSQL_HS_FAILED;
		conn->status = PGBENCH_CONN_BAD;
		return false;
	}

	if (pkt[0] == 0xFE)
	{
		const uint8 *p = pkt + 1;
		const uint8 *end = pkt + pkt_len;
		const uint8 *nul = memchr(p, '\0', end - p);
		PQExpBufferData token_buf;

		if (nul != NULL)
		{
			size_t		plen = (size_t) (nul - p);

			if (plen < sizeof(conn->auth_plugin))
			{
				memcpy(conn->auth_plugin, p, plen);
				conn->auth_plugin[plen] = '\0';
			}
			p = nul + 1;
			if (end - p >= 20)
				memcpy(conn->scramble, p, 20);
		}

		initPQExpBuffer(&token_buf);
		mysql_build_auth_token(conn->auth_plugin, conn->password, conn->scramble, &token_buf);
		conn->hs_seq_id = seq_id + 1;
		mysql_queue_packet(conn, conn->hs_seq_id, token_buf.data, token_buf.len);
		termPQExpBuffer(&token_buf);
		conn->hs_state = MYSQL_HS_WRITE_AUTH;
		return true;
	}

	if (pkt[0] == 0x01 && pkt_len >= 2)
	{
		if (pkt[1] == 0x03)
			return true;
		if (pkt[1] == 0x04)
		{
			const char *pw = conn->password ? conn->password : "";

			conn->hs_seq_id = seq_id + 1;
			mysql_queue_packet(conn, conn->hs_seq_id, pw, strlen(pw) + 1);
			conn->hs_state = MYSQL_HS_WRITE_AUTH;
			return true;
		}
	}

	printfPQExpBuffer(&conn->err_buf,
					  "unexpected MySQL auth packet header 0x%02x\n", pkt[0]);
	conn->hs_state = MYSQL_HS_FAILED;
	conn->status = PGBENCH_CONN_BAD;
	return false;
}

/*-------------------------------------------------------------------------
 * Query / Prepared Statement Response Packet Decoding
 *-------------------------------------------------------------------------
 */

static void
mysql_parse_err_packet(PgBenchConn *conn, const uint8 *pkt, size_t pkt_len,
					   PgBenchResult **res_out)
{
	uint16		err_code = 0;
	char		sqlstate[6] = "XX000";
	const char *msg = "MySQL error";
	size_t		mlen = strlen(msg);
	PgBenchResult *res;

	if (pkt_len >= 3)
	{
		err_code = mysql_read_u16_le(pkt + 1);
		if (pkt_len >= 9 && pkt[3] == '#')
		{
			memcpy(sqlstate, pkt + 4, 5);
			sqlstate[5] = '\0';
			msg = (const char *) (pkt + 9);
			mlen = pkt_len - 9;
		}
		else
		{
			msg = (const char *) (pkt + 3);
			mlen = pkt_len - 3;
		}
	}

	/*
	 * Map MySQL deadlock (1213 ER_LOCK_DEADLOCK) and lock wait timeout
	 * (1205 ER_LOCK_WAIT_TIMEOUT) to SQLSTATE "40001" so pgbench retries.
	 */
	if (err_code == 1213 || err_code == 1205 || strcmp(sqlstate, "40001") == 0)
		strcpy(sqlstate, "40001");
	else if (err_code == 1146)
		strcpy(sqlstate, "42P01");

	res = mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);
	memcpy(res->sqlstate, sqlstate, 6);
	res->err_msg = psprintf("ERROR %u (%s): %.*s\n",
							(unsigned int) err_code, sqlstate, (int) mlen, msg);
	printfPQExpBuffer(&conn->err_buf, "%s", res->err_msg);
	*res_out = res;
}

static void
mysql_update_tx_status_from_flags(PgBenchConn *conn, uint16 status_flags)
{
	if (status_flags & MYSQL_SERVER_STATUS_IN_TRANS)
		conn->tx_status = PGBENCH_TX_INTRANS;
	else
		conn->tx_status = PGBENCH_TX_IDLE;
}

static void
mysql_parse_ok_packet(PgBenchConn *conn, const uint8 *pkt, size_t pkt_len,
					  PgBenchResult **res_out, bool *more_results_out)
{
	const uint8 *p = pkt + 1;
	const uint8 *end = pkt + pkt_len;
	uint64		affected_rows = 0;
	uint64		last_insert_id = 0;
	PgBenchResult *res;

	if (more_results_out)
		*more_results_out = false;

	if (mysql_read_lenenc_int(&p, end, &affected_rows, NULL) &&
		mysql_read_lenenc_int(&p, end, &last_insert_id, NULL))
	{
		if (end - p >= 2)
		{
			uint16		status_flags = mysql_read_u16_le(p);

			mysql_update_tx_status_from_flags(conn, status_flags);
			if (more_results_out && (status_flags & MYSQL_SERVER_MORE_RESULTS_EXISTS))
				*more_results_out = true;
		}
	}

	res = mysql_alloc_result(PGBENCH_RES_COMMAND_OK);
	res->nrows = (int) affected_rows;
	*res_out = res;
}

static void
mysql_parse_col_def(const uint8 *pkt, size_t pkt_len,
					char **name_out, uint8 *type_out, uint16 *flags_out)
{
	const uint8 *p = pkt;
	const uint8 *end = pkt + pkt_len;
	uint64		slen = 0;
	bool		is_null = false;

	*name_out = NULL;
	*type_out = MYSQL_TYPE_VAR_STRING;
	*flags_out = 0;

	for (int i = 0; i < 4; i++)
	{
		if (!mysql_read_lenenc_int(&p, end, &slen, &is_null) || (size_t) (end - p) < slen)
			goto fallback;
		p += slen;
	}

	if (!mysql_read_lenenc_int(&p, end, &slen, &is_null) || (size_t) (end - p) < slen)
		goto fallback;
	*name_out = pnstrdup((const char *) p, (size_t) slen);
	p += slen;

	if (!mysql_read_lenenc_int(&p, end, &slen, &is_null) || (size_t) (end - p) < slen)
		return;
	p += slen;

	if (!mysql_read_lenenc_int(&p, end, &slen, &is_null) || (size_t) (end - p) < 10)
		return;

	*type_out = p[6];
	*flags_out = mysql_read_u16_le(p + 7);
	return;

fallback:
	if (*name_out == NULL)
		*name_out = pg_strdup("?column?");
}

static void
mysql_ensure_row_capacity(PgBenchResult *res)
{
	if (res->nrows >= res->row_cap)
	{
		int			new_cap = res->row_cap ? res->row_cap * 2 : 16;

		res->cells = pg_realloc_array(res->cells, char **, new_cap);
		res->is_null = pg_realloc_array(res->is_null, bool *, new_cap);
		res->row_cap = new_cap;
	}
}

static void
mysql_parse_text_row(PgBenchResult *res, const uint8 *pkt, size_t pkt_len)
{
	const uint8 *p = pkt;
	const uint8 *end = pkt + pkt_len;
	int			r = res->nrows;

	mysql_ensure_row_capacity(res);
	res->cells[r] = pg_malloc0_array(char *, res->ncols);
	res->is_null[r] = pg_malloc0_array(bool, res->ncols);

	for (int c = 0; c < res->ncols; c++)
	{
		uint64		slen = 0;
		bool		is_null = false;

		if (!mysql_read_lenenc_int(&p, end, &slen, &is_null))
		{
			res->is_null[r][c] = true;
			res->cells[r][c] = pg_strdup("");
			continue;
		}
		if (is_null)
		{
			res->is_null[r][c] = true;
			res->cells[r][c] = pg_strdup("");
		}
		else
		{
			if ((size_t) (end - p) < slen)
				slen = (uint64) (end - p);
			res->is_null[r][c] = false;
			res->cells[r][c] = pnstrdup((const char *) p, (size_t) slen);
			p += slen;
		}
	}
	res->nrows++;
}

static void
mysql_parse_binary_row(PgBenchResult *res, const uint8 *pkt, size_t pkt_len)
{
	const uint8 *p;
	const uint8 *end = pkt + pkt_len;
	const uint8 *null_bitmap;
	size_t		null_bm_len = (size_t) ((res->ncols + 7 + 2) / 8);
	int			r;

	if (pkt_len < 1 + null_bm_len || pkt[0] != 0x00)
	{
		mysql_parse_text_row(res, pkt, pkt_len);
		return;
	}

	null_bitmap = pkt + 1;
	p = pkt + 1 + null_bm_len;
	r = res->nrows;

	mysql_ensure_row_capacity(res);
	res->cells[r] = pg_malloc0_array(char *, res->ncols);
	res->is_null[r] = pg_malloc0_array(bool, res->ncols);

	for (int c = 0; c < res->ncols; c++)
	{
		int			byte_pos = (c + 2) / 8;
		int			bit_pos = (c + 2) % 8;
		uint8		col_type = res->col_types ? res->col_types[c] : MYSQL_TYPE_VAR_STRING;
		bool		is_unsigned = res->col_flags ? ((res->col_flags[c] & MYSQL_FLAG_UNSIGNED) != 0) : false;

		if (null_bitmap[byte_pos] & (1U << bit_pos))
		{
			res->is_null[r][c] = true;
			res->cells[r][c] = pg_strdup("");
			continue;
		}

		res->is_null[r][c] = false;
		switch (col_type)
		{
			case MYSQL_TYPE_NULL:
				res->is_null[r][c] = true;
				res->cells[r][c] = pg_strdup("");
				break;
			case MYSQL_TYPE_TINY:
				if (end - p >= 1)
				{
					if (is_unsigned)
						res->cells[r][c] = psprintf("%u", (unsigned int) p[0]);
					else
						res->cells[r][c] = psprintf("%d", (int) (int8) p[0]);
					p += 1;
				}
				break;
			case MYSQL_TYPE_SHORT:
			case MYSQL_TYPE_YEAR:
				if (end - p >= 2)
				{
					uint16		v = mysql_read_u16_le(p);

					if (is_unsigned || col_type == MYSQL_TYPE_YEAR)
						res->cells[r][c] = psprintf("%u", (unsigned int) v);
					else
						res->cells[r][c] = psprintf("%d", (int) (int16) v);
					p += 2;
				}
				break;
			case MYSQL_TYPE_LONG:
			case MYSQL_TYPE_INT24:
				if (end - p >= 4)
				{
					uint32		v = mysql_read_u32_le(p);

					if (is_unsigned)
						res->cells[r][c] = psprintf("%u", v);
					else
						res->cells[r][c] = psprintf("%d", (int32) v);
					p += 4;
				}
				break;
			case MYSQL_TYPE_LONGLONG:
				if (end - p >= 8)
				{
					uint64		v = mysql_read_u64_le(p);

					if (is_unsigned)
						res->cells[r][c] = psprintf(UINT64_FORMAT, v);
					else
						res->cells[r][c] = psprintf(INT64_FORMAT, (int64) v);
					p += 8;
				}
				break;
			case MYSQL_TYPE_FLOAT:
				if (end - p >= 4)
				{
					float		fv;

					memcpy(&fv, p, 4);
					res->cells[r][c] = psprintf("%.7g", (double) fv);
					p += 4;
				}
				break;
			case MYSQL_TYPE_DOUBLE:
				if (end - p >= 8)
				{
					double		dv;

					memcpy(&dv, p, 8);
					res->cells[r][c] = psprintf("%.15g", dv);
					p += 8;
				}
				break;
			case MYSQL_TYPE_DATE:
			case MYSQL_TYPE_DATETIME:
			case MYSQL_TYPE_TIMESTAMP:
				if (end - p >= 1)
				{
					uint8		dlen = *p++;

					if ((size_t) (end - p) >= dlen)
					{
						if (dlen == 0)
							res->cells[r][c] = pg_strdup("0000-00-00");
						else if (dlen == 4)
							res->cells[r][c] = psprintf("%04u-%02u-%02u",
														mysql_read_u16_le(p), p[2], p[3]);
						else if (dlen == 7)
							res->cells[r][c] = psprintf("%04u-%02u-%02u %02u:%02u:%02u",
														mysql_read_u16_le(p), p[2], p[3],
														p[4], p[5], p[6]);
						else if (dlen >= 11)
							res->cells[r][c] = psprintf("%04u-%02u-%02u %02u:%02u:%02u.%06u",
														mysql_read_u16_le(p), p[2], p[3],
														p[4], p[5], p[6],
														mysql_read_u32_le(p + 7));
						p += dlen;
					}
				}
				break;
			case MYSQL_TYPE_TIME:
				if (end - p >= 1)
				{
					uint8		tlen = *p++;

					if ((size_t) (end - p) >= tlen)
					{
						if (tlen == 0)
							res->cells[r][c] = pg_strdup("00:00:00");
						else if (tlen >= 8)
						{
							const char *sign = p[0] ? "-" : "";
							uint32		days = mysql_read_u32_le(p + 1);
							uint32		hours = days * 24U + p[5];

							if (tlen >= 12)
								res->cells[r][c] = psprintf("%s%02u:%02u:%02u.%06u",
															sign, hours, p[6], p[7],
															mysql_read_u32_le(p + 8));
							else
								res->cells[r][c] = psprintf("%s%02u:%02u:%02u",
															sign, hours, p[6], p[7]);
						}
						p += tlen;
					}
				}
				break;
			default:
				{
					uint64		slen = 0;
					bool		isnull = false;

					if (mysql_read_lenenc_int(&p, end, &slen, &isnull) && !isnull)
					{
						if ((size_t) (end - p) < slen)
							slen = (uint64) (end - p);
						res->cells[r][c] = pnstrdup((const char *) p, (size_t) slen);
						p += slen;
					}
				}
				break;
		}
		if (res->cells[r][c] == NULL)
			res->cells[r][c] = pg_strdup("");
	}
	res->nrows++;
}

static void
mysql_finish_command_with_result(PgBenchConn *conn, PgBenchResult *res)
{
	if (conn->pipe_status != PGBENCH_PIPE_OFF && conn->pipeline_aborted)
	{
		mysql_free_result(res);
		res = mysql_alloc_result(PGBENCH_RES_PIPELINE_ABORTED);
	}
	else if (res->status == PGBENCH_RES_FATAL_ERROR &&
			 conn->pipe_status != PGBENCH_PIPE_OFF)
	{
		conn->pipeline_aborted = true;
		conn->pipe_status = PGBENCH_PIPE_ABORTED;
	}

	mysql_push_result(conn, res);
	mysql_pop_pending(conn);

	while (conn->pending_head != NULL)
	{
		if (conn->pending_head->type == MYSQL_PENDING_SYNC)
		{
			conn->pipeline_aborted = false;
			if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
				conn->pipe_status = PGBENCH_PIPE_ON;
			mysql_push_result(conn, mysql_alloc_result(PGBENCH_RES_PIPELINE_SYNC));
			mysql_pop_pending(conn);
		}
		else if (conn->pending_head->type == MYSQL_PENDING_LOCAL_OK)
		{
			PgBenchResult *lres = mysql_alloc_result(
				(conn->pipe_status != PGBENCH_PIPE_OFF && conn->pipeline_aborted) ?
				PGBENCH_RES_PIPELINE_ABORTED : PGBENCH_RES_COMMAND_OK);

			mysql_push_result(conn, lres);
			mysql_pop_pending(conn);
		}
		else
		{
			break;
		}
	}
}

static void
mysql_process_command_packet(PgBenchConn *conn, const uint8 *pkt, size_t pkt_len)
{
	MysqlPendingCmd *cmd = conn->pending_head;

	if (cmd == NULL)
		return;

	if (conn->read_phase == MYSQL_PHASE_FIRST_PKT)
	{
		if (cmd->type == MYSQL_PENDING_PREPARE)
		{
			if (pkt_len >= 1 && pkt[0] == 0xFF)
			{
				uint16		err_code = (pkt_len >= 3) ? mysql_read_u16_le(pkt + 1) : 0;

				if (err_code == 1295)
				{
					mysql_store_prepared(conn, cmd->stmt_name, cmd->translated_sql,
										 0, cmd->nmap, 0,
										 cmd->param_map, cmd->nmap, true);
					mysql_finish_command_with_result(conn,
													 mysql_alloc_result(PGBENCH_RES_COMMAND_OK));
					return;
				}
				else
				{
					PgBenchResult *err_res = NULL;

					mysql_parse_err_packet(conn, pkt, pkt_len, &err_res);
					mysql_finish_command_with_result(conn, err_res);
					return;
				}
			}

			if (pkt_len >= 12 && pkt[0] == 0x00)
			{
				conn->prep_stmt_id = mysql_read_u32_le(pkt + 1);
				conn->prep_num_cols = (int) mysql_read_u16_le(pkt + 5);
				conn->prep_num_params = (int) mysql_read_u16_le(pkt + 7);
				conn->prep_params_rem = conn->prep_num_params;
				conn->prep_cols_rem = conn->prep_num_cols;

				mysql_store_prepared(conn, cmd->stmt_name, cmd->translated_sql,
									 conn->prep_stmt_id, conn->prep_num_params,
									 conn->prep_num_cols, cmd->param_map, cmd->nmap,
									 false);

				if (conn->prep_params_rem > 0)
					conn->read_phase = MYSQL_PHASE_PREPARE_PARAMS;
				else if (conn->prep_cols_rem > 0)
					conn->read_phase = MYSQL_PHASE_PREPARE_COLS;
				else
					mysql_finish_command_with_result(conn,
													 mysql_alloc_result(PGBENCH_RES_COMMAND_OK));
				return;
			}

			{
				PgBenchResult *err_res = mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);

				strcpy(err_res->sqlstate, "XX000");
				err_res->err_msg = pg_strdup("malformed COM_STMT_PREPARE response\n");
				mysql_finish_command_with_result(conn, err_res);
				return;
			}
		}

		if (pkt_len >= 1 && pkt[0] == 0xFF)
		{
			PgBenchResult *err_res = NULL;

			mysql_parse_err_packet(conn, pkt, pkt_len, &err_res);
			mysql_finish_command_with_result(conn, err_res);
			return;
		}

		if (pkt_len >= 1 && pkt[0] == 0x00)
		{
			PgBenchResult *ok_res = NULL;
			bool		more_results = false;

			mysql_parse_ok_packet(conn, pkt, pkt_len, &ok_res, &more_results);
			if (more_results)
			{
				mysql_free_result(ok_res);
				conn->read_phase = MYSQL_PHASE_FIRST_PKT;
				return;
			}
			mysql_finish_command_with_result(conn, ok_res);
			return;
		}

		{
			const uint8 *p = pkt;
			const uint8 *end = pkt + pkt_len;
			uint64		ncols = 0;

			if (!mysql_read_lenenc_int(&p, end, &ncols, NULL) || ncols == 0 || ncols > 4096)
			{
				PgBenchResult *err_res = mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);

				strcpy(err_res->sqlstate, "XX000");
				err_res->err_msg = pg_strdup("invalid column count in MySQL resultset\n");
				mysql_finish_command_with_result(conn, err_res);
				return;
			}

			conn->cols_expected = (int) ncols;
			conn->cols_read = 0;
			conn->building_res = mysql_alloc_result(PGBENCH_RES_TUPLES_OK);
			conn->building_res->ncols = (int) ncols;
			conn->building_res->col_names = pg_malloc0_array(char *, ncols);
			conn->building_res->col_types = pg_malloc0_array(uint8, ncols);
			conn->building_res->col_flags = pg_malloc0_array(uint16, ncols);
			conn->read_phase = MYSQL_PHASE_COLS;
			return;
		}
	}

	if (conn->read_phase == MYSQL_PHASE_PREPARE_PARAMS)
	{
		if (conn->prep_params_rem > 0)
		{
			conn->prep_params_rem--;
			return;
		}
		if (conn->prep_cols_rem > 0)
			conn->read_phase = MYSQL_PHASE_PREPARE_COLS;
		else
			mysql_finish_command_with_result(conn,
											 mysql_alloc_result(PGBENCH_RES_COMMAND_OK));
		return;
	}

	if (conn->read_phase == MYSQL_PHASE_PREPARE_COLS)
	{
		if (conn->prep_cols_rem > 0)
		{
			conn->prep_cols_rem--;
			return;
		}
		mysql_finish_command_with_result(conn,
										 mysql_alloc_result(PGBENCH_RES_COMMAND_OK));
		return;
	}

	if (conn->read_phase == MYSQL_PHASE_COLS)
	{
		if (conn->cols_read < conn->cols_expected)
		{
			int			idx = conn->cols_read++;

			mysql_parse_col_def(pkt, pkt_len,
								&conn->building_res->col_names[idx],
								&conn->building_res->col_types[idx],
								&conn->building_res->col_flags[idx]);
			return;
		}

		if (pkt_len < 9 && pkt[0] == 0xFE)
		{
			conn->read_phase = MYSQL_PHASE_ROWS;
			return;
		}

		conn->read_phase = MYSQL_PHASE_ROWS;
	}

	if (conn->read_phase == MYSQL_PHASE_ROWS)
	{
		if (pkt_len >= 1 && pkt[0] == 0xFF)
		{
			PgBenchResult *err_res = NULL;

			mysql_parse_err_packet(conn, pkt, pkt_len, &err_res);
			mysql_finish_command_with_result(conn, err_res);
			return;
		}

		if (pkt_len >= 1 && pkt[0] == 0xFE && pkt_len < 9)
		{
			bool		more_results = false;

			if (pkt_len >= 5)
			{
				uint16		status_flags = mysql_read_u16_le(pkt + 3);

				mysql_update_tx_status_from_flags(conn, status_flags);
				if (status_flags & MYSQL_SERVER_MORE_RESULTS_EXISTS)
					more_results = true;
			}
			{
				PgBenchResult *done_res = conn->building_res;

				conn->building_res = NULL;
				if (more_results)
				{
					mysql_free_result(done_res);
					conn->read_phase = MYSQL_PHASE_FIRST_PKT;
					return;
				}
				mysql_finish_command_with_result(conn, done_res);
			}
			return;
		}

		if (cmd->type == MYSQL_PENDING_STMT_EXECUTE && pkt_len >= 1 && pkt[0] == 0x00)
			mysql_parse_binary_row(conn->building_res, pkt, pkt_len);
		else
			mysql_parse_text_row(conn->building_res, pkt, pkt_len);
	}
}

static bool
mysql_parse_incoming_packets(PgBenchConn *conn)
{
	while (conn->in_len - conn->in_parsed >= 4)
	{
		const uint8 *hdr = conn->in_buf + conn->in_parsed;
		uint32		pkt_len = mysql_read_u24_le(hdr);
		uint8		seq_id = hdr[3];
		const uint8 *payload;
		size_t		payload_len;

		if (conn->in_len - conn->in_parsed < 4 + (size_t) pkt_len)
			break;

		conn->in_parsed += 4 + (size_t) pkt_len;

		if (pkt_len == 0xFFFFFFU)
		{
			appendBinaryPQExpBuffer(&conn->multipkt_buf,
									(const char *) (hdr + 4), pkt_len);
			conn->in_multipkt = true;
			continue;
		}
		else if (conn->in_multipkt)
		{
			if (pkt_len > 0)
				appendBinaryPQExpBuffer(&conn->multipkt_buf,
										(const char *) (hdr + 4), pkt_len);
			conn->in_multipkt = false;
			payload = (const uint8 *) conn->multipkt_buf.data;
			payload_len = conn->multipkt_buf.len;
		}
		else
		{
			payload = hdr + 4;
			payload_len = pkt_len;
		}

		if (conn->hs_state == MYSQL_HS_READ_GREETING)
		{
			if (!mysql_handle_greeting_packet(conn, payload, payload_len, seq_id))
			{
				resetPQExpBuffer(&conn->multipkt_buf);
				return false;
			}
		}
		else if (conn->hs_state == MYSQL_HS_READ_AUTH_RESULT)
		{
			if (!mysql_handle_auth_result_packet(conn, payload, payload_len, seq_id))
			{
				resetPQExpBuffer(&conn->multipkt_buf);
				return false;
			}
		}
		else
		{
			mysql_process_command_packet(conn, payload, payload_len);
		}

		if (conn->multipkt_buf.len > 0)
			resetPQExpBuffer(&conn->multipkt_buf);
	}

	if (conn->in_parsed > 0)
	{
		if (conn->in_parsed == conn->in_len)
		{
			conn->in_parsed = 0;
			conn->in_len = 0;
		}
		else if (conn->in_parsed > 65536)
		{
			size_t		rem = conn->in_len - conn->in_parsed;

			memmove(conn->in_buf, conn->in_buf + conn->in_parsed, rem);
			conn->in_len = rem;
			conn->in_parsed = 0;
		}
	}

	return true;
}

/*-------------------------------------------------------------------------
 * Connection Lifecycle Implementation
 *-------------------------------------------------------------------------
 */

static bool
mysql_init_global(const char *conninfo, int nthreads_arg, int nclients_arg)
{
	(void) nthreads_arg;
	(void) nclients_arg;
	pg_free(saved_mysql_conninfo);
	saved_mysql_conninfo = conninfo ? pg_strdup(conninfo) : NULL;
	return true;
}

static void
mysql_shutdown_global(void)
{
	pg_free(saved_mysql_conninfo);
	saved_mysql_conninfo = NULL;
}

static void
mysql_set_cancel_conn(PgBenchConn *conn)
{
	(void) conn;
}

static void
mysql_reset_cancel_conn(void)
{
}

static PgBenchConn *
mysql_connect_start(const char *host, const char *port,
					const char *user, const char *dbname)
{
	PgBenchConn *conn = pg_malloc0_object(PgBenchConn);
	const char *env_val;
	struct addrinfo hints;
	struct addrinfo *addrs = NULL;
	struct addrinfo *rp;
	int			rc;

	conn->sock = -1;
	conn->status = PGBENCH_CONN_BAD;
	conn->tx_status = PGBENCH_TX_IDLE;
	conn->pipe_status = PGBENCH_PIPE_OFF;
	initPQExpBuffer(&conn->err_buf);
	initPQExpBuffer(&conn->multipkt_buf);

	env_val = getenv("MYSQL_HOST");
	conn->host = pg_strdup((env_val && *env_val) ? env_val : "127.0.0.1");

	env_val = getenv("MYSQL_TCP_PORT");
	if (env_val == NULL || *env_val == '\0')
		env_val = getenv("MYSQL_PORT");
	conn->port = pg_strdup((env_val && *env_val) ? env_val : MYSQL_DEFAULT_PORT);

	env_val = getenv("MYSQL_USER");
	conn->user = pg_strdup((env_val && *env_val) ? env_val : "root");

	env_val = getenv("MYSQL_PWD");
	conn->password = pg_strdup(env_val ? env_val : "");

	env_val = getenv("MYSQL_DATABASE");
	conn->dbname = pg_strdup(env_val ? env_val : "");

	if (saved_mysql_conninfo && *saved_mysql_conninfo)
		mysql_parse_uri(saved_mysql_conninfo, &conn->host, &conn->port,
						&conn->user, &conn->password, &conn->dbname);

	if (dbname && *dbname)
	{
		if (pg_strncasecmp(dbname, "mysql://", 8) == 0 ||
			pg_strncasecmp(dbname, "mariadb://", 10) == 0)
		{
			mysql_parse_uri(dbname, &conn->host, &conn->port,
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
		if (pg_strncasecmp(host, "mysql://", 8) == 0 ||
			pg_strncasecmp(host, "mariadb://", 10) == 0)
		{
			mysql_parse_uri(host, &conn->host, &conn->port,
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

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	rc = getaddrinfo(conn->host, conn->port, &hints, &addrs);
	if (rc != 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not resolve MySQL host \"%s:%s\": %s\n",
						  conn->host, conn->port, gai_strerror(rc));
		conn->hs_state = MYSQL_HS_FAILED;
		return conn;
	}

	for (rp = addrs; rp != NULL; rp = rp->ai_next)
	{
		int			s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		int			flags;
		int			one = 1;

		if (s < 0)
			continue;

		flags = fcntl(s, F_GETFL, 0);
		if (flags >= 0)
			(void) fcntl(s, F_SETFL, flags | O_NONBLOCK);
		(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
		{
			conn->sock = s;
			conn->status = PGBENCH_CONN_POLLING_READING;
			conn->hs_state = MYSQL_HS_READ_GREETING;
			break;
		}
		else if (errno == EINPROGRESS)
		{
			conn->sock = s;
			conn->status = PGBENCH_CONN_POLLING_WRITING;
			conn->hs_state = MYSQL_HS_TCP_CONNECTING;
			break;
		}

		close(s);
	}

	if (conn->sock < 0)
	{
		printfPQExpBuffer(&conn->err_buf,
						  "could not connect to MySQL server at \"%s:%s\": %s\n",
						  conn->host, conn->port, strerror(errno));
		conn->hs_state = MYSQL_HS_FAILED;
		conn->status = PGBENCH_CONN_BAD;
	}

	freeaddrinfo(addrs);
	return conn;
}

static bool mysql_consume_input(PgBenchConn *conn);

static PgBenchConnStatus
mysql_connect_poll(PgBenchConn *conn)
{
	if (conn == NULL || conn->sock < 0 || conn->hs_state == MYSQL_HS_FAILED)
	{
		if (conn)
			conn->status = PGBENCH_CONN_BAD;
		return PGBENCH_CONN_POLLING_FAILED;
	}

	if (conn->hs_state == MYSQL_HS_DONE)
	{
		conn->status = PGBENCH_CONN_OK;
		return PGBENCH_CONN_POLLING_OK;
	}

	if (conn->hs_state == MYSQL_HS_TCP_CONNECTING)
	{
		int			so_err = 0;
		socklen_t	slen = sizeof(so_err);

		if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 ||
			so_err != 0)
		{
			printfPQExpBuffer(&conn->err_buf,
							  "could not connect to MySQL server at \"%s:%s\": %s\n",
							  conn->host, conn->port,
							  strerror(so_err ? so_err : errno));
			conn->hs_state = MYSQL_HS_FAILED;
			conn->status = PGBENCH_CONN_BAD;
			return PGBENCH_CONN_POLLING_FAILED;
		}
		conn->hs_state = MYSQL_HS_READ_GREETING;
		return PGBENCH_CONN_POLLING_READING;
	}

	if (conn->hs_state == MYSQL_HS_WRITE_AUTH)
	{
		if (!mysql_flush_out(conn))
		{
			conn->hs_state = MYSQL_HS_FAILED;
			return PGBENCH_CONN_POLLING_FAILED;
		}
		if (conn->out_sent < conn->out_len)
			return PGBENCH_CONN_POLLING_WRITING;
		conn->hs_state = MYSQL_HS_READ_AUTH_RESULT;
		return PGBENCH_CONN_POLLING_READING;
	}

	if (conn->hs_state == MYSQL_HS_READ_GREETING ||
		conn->hs_state == MYSQL_HS_READ_AUTH_RESULT)
	{
		if (!mysql_consume_input(conn))
		{
			conn->hs_state = MYSQL_HS_FAILED;
			conn->status = PGBENCH_CONN_BAD;
			return PGBENCH_CONN_POLLING_FAILED;
		}

		if (conn->hs_state == MYSQL_HS_FAILED)
		{
			conn->status = PGBENCH_CONN_BAD;
			return PGBENCH_CONN_POLLING_FAILED;
		}

		if (conn->hs_state == MYSQL_HS_WRITE_AUTH)
		{
			if (!mysql_flush_out(conn))
			{
				conn->hs_state = MYSQL_HS_FAILED;
				return PGBENCH_CONN_POLLING_FAILED;
			}
			if (conn->out_sent < conn->out_len)
				return PGBENCH_CONN_POLLING_WRITING;
			conn->hs_state = MYSQL_HS_READ_AUTH_RESULT;
			return PGBENCH_CONN_POLLING_READING;
		}

		if (conn->hs_state == MYSQL_HS_DONE)
		{
			conn->status = PGBENCH_CONN_OK;
			return PGBENCH_CONN_POLLING_OK;
		}

		return PGBENCH_CONN_POLLING_READING;
	}

	return PGBENCH_CONN_POLLING_FAILED;
}

static PgBenchConn *
mysql_connect_sync(const char *host, const char *port,
				   const char *user, const char *dbname)
{
	PgBenchConn *conn = mysql_connect_start(host, port, user, dbname);
	PgBenchConnStatus pstatus;

	if (conn == NULL || conn->hs_state == MYSQL_HS_FAILED)
		return conn;

	pstatus = (conn->hs_state == MYSQL_HS_TCP_CONNECTING) ?
		PGBENCH_CONN_POLLING_WRITING : PGBENCH_CONN_POLLING_READING;

	while (pstatus != PGBENCH_CONN_POLLING_OK &&
		   pstatus != PGBENCH_CONN_POLLING_FAILED)
	{
		struct pollfd pfd;
		int			rc;

		pfd.fd = conn->sock;
		pfd.events = (pstatus == PGBENCH_CONN_POLLING_WRITING) ? POLLOUT : POLLIN;
		pfd.revents = 0;

		rc = poll(&pfd, 1, 10000);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			printfPQExpBuffer(&conn->err_buf,
							  "poll() failed during MySQL handshake: %s\n",
							  strerror(errno));
			conn->status = PGBENCH_CONN_BAD;
			break;
		}
		if (rc == 0)
		{
			printfPQExpBuffer(&conn->err_buf,
							  "timed out connecting to MySQL server at \"%s:%s\"\n",
							  conn->host, conn->port);
			conn->status = PGBENCH_CONN_BAD;
			break;
		}

		pstatus = mysql_connect_poll(conn);
	}

	return conn;
}

static void
mysql_finish(PgBenchConn *conn)
{
	if (conn == NULL)
		return;

	if (conn->sock >= 0)
	{
		if (conn->status == PGBENCH_CONN_OK)
		{
			uint8		quit_cmd = MYSQL_COM_QUIT;

			mysql_queue_packet(conn, 0, &quit_cmd, 1);
			(void) mysql_flush_out(conn);
		}
		close(conn->sock);
		conn->sock = -1;
	}

	while (conn->pending_head)
		mysql_pop_pending(conn);

	while (conn->res_head)
	{
		PgBenchResult *r = conn->res_head;

		conn->res_head = r->next;
		mysql_free_result(r);
	}
	conn->res_tail = NULL;

	if (conn->building_res)
		mysql_free_result(conn->building_res);

	for (int i = 0; i < conn->nprep_stmts; i++)
	{
		pg_free(conn->prep_stmts[i].name);
		pg_free(conn->prep_stmts[i].translated_sql);
		pg_free(conn->prep_stmts[i].param_map);
	}
	pg_free(conn->prep_stmts);

	pg_free(conn->host);
	pg_free(conn->port);
	pg_free(conn->dbname);
	pg_free(conn->user);
	pg_free(conn->password);
	pg_free(conn->out_buf);
	pg_free(conn->in_buf);
	termPQExpBuffer(&conn->err_buf);
	termPQExpBuffer(&conn->multipkt_buf);
	pg_free(conn);
}

static PgBenchConnStatus
mysql_status(const PgBenchConn *conn)
{
	return conn ? conn->status : PGBENCH_CONN_BAD;
}

static const char *
mysql_error_message(const PgBenchConn *conn)
{
	return (conn && conn->err_buf.data) ? conn->err_buf.data : "";
}

static int
mysql_get_poll_fd(const PgBenchConn *conn)
{
	return conn ? conn->sock : PGBENCH_POLL_ERROR;
}

static PgBenchTxStatus
mysql_tx_status(const PgBenchConn *conn)
{
	return conn ? conn->tx_status : PGBENCH_TX_UNKNOWN;
}

static const char *
mysql_get_host(const PgBenchConn *conn)
{
	return (conn && conn->host) ? conn->host : "";
}

static const char *
mysql_get_port(const PgBenchConn *conn)
{
	return (conn && conn->port) ? conn->port : "";
}

static const char *
mysql_get_db(const PgBenchConn *conn)
{
	return (conn && conn->dbname) ? conn->dbname : "";
}

/*-------------------------------------------------------------------------
 * Async & Sync Command Dispatch
 *-------------------------------------------------------------------------
 */

static bool
mysql_send_query(PgBenchConn *conn, const char *sql)
{
	bool		is_noop = false;
	char	   *adapted;
	PQExpBufferData pkt;

	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	adapted = mysql_adapt_sql(sql, &is_noop);
	if (is_noop)
	{
		pg_free(adapted);
		if (conn->pending_head == NULL)
		{
			PgBenchResult *res = mysql_alloc_result(
				(conn->pipe_status != PGBENCH_PIPE_OFF && conn->pipeline_aborted) ?
				PGBENCH_RES_PIPELINE_ABORTED : PGBENCH_RES_COMMAND_OK);

			mysql_push_result(conn, res);
		}
		else
		{
			mysql_push_pending(conn, MYSQL_PENDING_LOCAL_OK, NULL, NULL, NULL, 0);
		}
		return true;
	}

	initPQExpBuffer(&pkt);
	appendPQExpBufferChar(&pkt, (char) MYSQL_COM_QUERY);
	appendPQExpBufferStr(&pkt, adapted);
	pg_free(adapted);

	mysql_queue_packet(conn, 0, pkt.data, pkt.len);
	termPQExpBuffer(&pkt);

	mysql_push_pending(conn, MYSQL_PENDING_QUERY, NULL, NULL, NULL, 0);
	return mysql_flush_out(conn);
}

static bool
mysql_send_query_params(PgBenchConn *conn, const char *sql, int nparams,
						const char *const *param_texts,
						const struct Variable *const *param_vars)
{
	bool		is_noop = false;
	char	   *adapted;
	int		   *param_map = NULL;
	int			nmap = 0;
	char	   *translated;
	char	   *bound_sql;
	bool		rc;

	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	adapted = mysql_adapt_sql(sql, &is_noop);
	if (is_noop)
	{
		pg_free(adapted);
		return mysql_send_query(conn, "VACUUM");
	}

	translated = mysql_translate_dollar_params(adapted, &param_map, &nmap);
	bound_sql = mysql_bind_qmark_literals(translated, param_map, nmap,
										  nparams, param_texts, param_vars);

	pg_free(adapted);
	pg_free(translated);
	pg_free(param_map);

	rc = mysql_send_query(conn, bound_sql);
	pg_free(bound_sql);
	return rc;
}

static bool
mysql_send_prepare(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	bool		is_noop = false;
	char	   *adapted;
	int		   *param_map = NULL;
	int			nmap = 0;
	char	   *translated;
	PQExpBufferData pkt;

	(void) nparams;
	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	adapted = mysql_adapt_sql(sql, &is_noop);
	if (is_noop)
	{
		pg_free(adapted);
		mysql_store_prepared(conn, stmt_name, "", 0, 0, 0, NULL, 0, true);
		if (conn->pending_head == NULL)
			mysql_push_result(conn, mysql_alloc_result(PGBENCH_RES_COMMAND_OK));
		else
			mysql_push_pending(conn, MYSQL_PENDING_LOCAL_OK, NULL, NULL, NULL, 0);
		return true;
	}

	translated = mysql_translate_dollar_params(adapted, &param_map, &nmap);
	pg_free(adapted);

	initPQExpBuffer(&pkt);
	appendPQExpBufferChar(&pkt, (char) MYSQL_COM_STMT_PREPARE);
	appendPQExpBufferStr(&pkt, translated);

	mysql_queue_packet(conn, 0, pkt.data, pkt.len);
	termPQExpBuffer(&pkt);

	mysql_push_pending(conn, MYSQL_PENDING_PREPARE, stmt_name,
					   translated, param_map, nmap);
	pg_free(translated);
	pg_free(param_map);

	return mysql_flush_out(conn);
}

static bool
mysql_send_query_prepared(PgBenchConn *conn, const char *stmt_name, int nparams,
						  const char *const *param_texts,
						  const struct Variable *const *param_vars)
{
	MysqlPreparedStmt *stmt;
	PQExpBufferData pkt;
	PQExpBufferData types_buf;
	PQExpBufferData vals_buf;
	int			num_slots;
	size_t		null_bm_len;
	uint8	   *null_bm;

	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;

	stmt = mysql_find_prepared(conn, stmt_name);
	if (stmt == NULL)
	{
		PgBenchResult *err_res = mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);

		strcpy(err_res->sqlstate, "26000");
		err_res->err_msg = psprintf("prepared statement \"%s\" does not exist\n",
									stmt_name ? stmt_name : "");
		printfPQExpBuffer(&conn->err_buf, "%s", err_res->err_msg);
		mysql_push_result(conn, err_res);
		return true;
	}

	if (stmt->client_emulated)
	{
		char	   *bound_sql;
		bool		rc;

		if (stmt->translated_sql[0] == '\0')
			return mysql_send_query(conn, "VACUUM");

		bound_sql = mysql_bind_qmark_literals(stmt->translated_sql,
											  stmt->param_map, stmt->nmap,
											  nparams, param_texts, param_vars);
		rc = mysql_send_query(conn, bound_sql);
		pg_free(bound_sql);
		return rc;
	}

	num_slots = stmt->num_params;
	initPQExpBuffer(&pkt);
	appendPQExpBufferChar(&pkt, (char) MYSQL_COM_STMT_EXECUTE);

	{
		uint8		hdr[9];

		mysql_write_u32_le(hdr, stmt->stmt_id);
		hdr[4] = 0x00;			/* flags = CURSOR_TYPE_NO_CURSOR */
		mysql_write_u32_le(hdr + 5, 1); /* iteration_count = 1 */
		appendBinaryPQExpBuffer(&pkt, (const char *) hdr, 9);
	}

	if (num_slots > 0)
	{
		null_bm_len = (size_t) ((num_slots + 7) / 8);
		null_bm = pg_malloc0(null_bm_len);
		initPQExpBuffer(&types_buf);
		initPQExpBuffer(&vals_buf);

		for (int i = 0; i < num_slots; i++)
		{
			int			src_idx = (stmt->param_map && i < stmt->nmap) ? stmt->param_map[i] : i;
			uint8		tbytes[2] = {MYSQL_TYPE_VAR_STRING, 0x00};
			bool		handled = false;

			if (src_idx < 0 || src_idx >= nparams)
			{
				null_bm[i / 8] |= (uint8) (1U << (i % 8));
				tbytes[0] = MYSQL_TYPE_NULL;
				appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
				continue;
			}

			if (param_vars != NULL && param_vars[src_idx] != NULL)
			{
				const PgBenchValue *v = &param_vars[src_idx]->value;

				switch (v->type)
				{
					case PGBT_NULL:
						null_bm[i / 8] |= (uint8) (1U << (i % 8));
						tbytes[0] = MYSQL_TYPE_NULL;
						appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
						handled = true;
						break;
					case PGBT_INT:
						{
							uint8		ib[8];

							tbytes[0] = MYSQL_TYPE_LONGLONG;
							appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
							mysql_write_u64_le(ib, (uint64) v->u.ival);
							appendBinaryPQExpBuffer(&vals_buf, (const char *) ib, 8);
							handled = true;
						}
						break;
					case PGBT_DOUBLE:
						{
							uint8		db[8];

							tbytes[0] = MYSQL_TYPE_DOUBLE;
							appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
							memcpy(db, &v->u.dval, 8);
							appendBinaryPQExpBuffer(&vals_buf, (const char *) db, 8);
							handled = true;
						}
						break;
					case PGBT_BOOLEAN:
						{
							uint8		bb = v->u.bval ? 1 : 0;

							tbytes[0] = MYSQL_TYPE_TINY;
							appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
							appendBinaryPQExpBuffer(&vals_buf, (const char *) &bb, 1);
							handled = true;
						}
						break;
					case PGBT_BYTEA:
						{
							tbytes[0] = MYSQL_TYPE_BLOB;
							appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
							mysql_append_lenenc_str(&vals_buf,
													(const char *) v->u.byval.data,
													(size_t) v->u.byval.len);
							handled = true;
						}
						break;
					default:
						break;
				}
			}

			if (!handled)
			{
				const char *s = (param_texts != NULL) ? param_texts[src_idx] : NULL;

				if (s == NULL && param_vars != NULL && param_vars[src_idx] != NULL &&
					param_vars[src_idx]->value.type == PGBT_TEXT)
					s = param_vars[src_idx]->value.u.sval;

				if (s == NULL)
				{
					null_bm[i / 8] |= (uint8) (1U << (i % 8));
					tbytes[0] = MYSQL_TYPE_NULL;
					appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
				}
				else
				{
					tbytes[0] = MYSQL_TYPE_VAR_STRING;
					appendBinaryPQExpBuffer(&types_buf, (const char *) tbytes, 2);
					mysql_append_lenenc_str(&vals_buf, s, strlen(s));
				}
			}
		}

		appendBinaryPQExpBuffer(&pkt, (const char *) null_bm, null_bm_len);
		appendPQExpBufferChar(&pkt, 0x01);	/* new_params_bound_flag = 1 */
		appendBinaryPQExpBuffer(&pkt, types_buf.data, types_buf.len);
		appendBinaryPQExpBuffer(&pkt, vals_buf.data, vals_buf.len);

		pg_free(null_bm);
		termPQExpBuffer(&types_buf);
		termPQExpBuffer(&vals_buf);
	}

	mysql_queue_packet(conn, 0, pkt.data, pkt.len);
	termPQExpBuffer(&pkt);

	mysql_push_pending(conn, MYSQL_PENDING_STMT_EXECUTE, stmt_name, NULL, NULL, 0);
	return mysql_flush_out(conn);
}

static bool
mysql_consume_input(PgBenchConn *conn)
{
	if (conn == NULL || conn->sock < 0)
		return false;

	if (conn->out_sent < conn->out_len)
	{
		if (!mysql_flush_out(conn))
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

		n = recv(conn->sock, conn->in_buf + conn->in_len,
				 conn->in_cap - conn->in_len, 0);
		if (n > 0)
		{
			conn->in_len += (size_t) n;
			if (!mysql_parse_incoming_packets(conn))
				return false;
		}
		else if (n == 0)
		{
			printfPQExpBuffer(&conn->err_buf,
							  "MySQL server closed the connection unexpectedly\n");
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
							  "MySQL socket recv failed: %s\n",
							  strerror(errno));
			conn->status = PGBENCH_CONN_BAD;
			return false;
		}
	}

	return true;
}

static bool
mysql_is_busy(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	if (conn->out_sent < conn->out_len)
		(void) mysql_flush_out(conn);
	if (conn->res_head != NULL)
		return false;
	if (conn->pending_head != NULL && !conn->result_drained)
		return true;
	return false;
}

static PgBenchResult *
mysql_get_result(PgBenchConn *conn)
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
			if (!mysql_flush_out(conn))
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
			printfPQExpBuffer(&conn->err_buf, "MySQL query timed out\n");
			conn->status = PGBENCH_CONN_BAD;
			break;
		}
		if (!mysql_consume_input(conn))
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
mysql_exec_sync(PgBenchConn *conn, const char *sql)
{
	PgBenchResult *last_res = NULL;
	PgBenchResult *res;

	if (!mysql_send_query(conn, sql))
		return mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);

	while ((res = mysql_get_result(conn)) != NULL || conn->pending_head != NULL)
	{
		if (res != NULL)
		{
			if (last_res)
				mysql_free_result(last_res);
			last_res = res;
		}
	}
	conn->result_drained = false;
	return last_res;
}

static PgBenchResult *
mysql_prepare_sync(PgBenchConn *conn, const char *stmt_name,
				   const char *sql, int nparams)
{
	PgBenchResult *res;

	if (!mysql_send_prepare(conn, stmt_name, sql, nparams))
		return mysql_alloc_result(PGBENCH_RES_FATAL_ERROR);

	res = mysql_get_result(conn);
	conn->result_drained = false;
	return res;
}

static char *
mysql_escape_identifier(PgBenchConn *conn, const char *str, size_t len)
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
mysql_free_mem(void *ptr)
{
	pg_free(ptr);
}

/*-------------------------------------------------------------------------
 * Pipeline Mode
 *-------------------------------------------------------------------------
 */

static bool
mysql_enter_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL || conn->status != PGBENCH_CONN_OK)
		return false;
	conn->pipe_status = PGBENCH_PIPE_ON;
	conn->pipeline_aborted = false;
	return true;
}

static bool
mysql_exit_pipeline_mode(PgBenchConn *conn)
{
	if (conn == NULL)
		return false;
	if (conn->pending_head != NULL || conn->res_head != NULL)
		return false;
	conn->pipe_status = PGBENCH_PIPE_OFF;
	conn->pipeline_aborted = false;
	conn->result_drained = false;
	return true;
}

static PgBenchPipelineStatus
mysql_pipeline_status(const PgBenchConn *conn)
{
	return conn ? conn->pipe_status : PGBENCH_PIPE_OFF;
}

static bool
mysql_send_pipeline_sync(PgBenchConn *conn)
{
	if (conn == NULL || conn->pipe_status == PGBENCH_PIPE_OFF)
		return false;

	if (conn->pending_head == NULL)
	{
		conn->pipeline_aborted = false;
		if (conn->pipe_status == PGBENCH_PIPE_ABORTED)
			conn->pipe_status = PGBENCH_PIPE_ON;
		mysql_push_result(conn, mysql_alloc_result(PGBENCH_RES_PIPELINE_SYNC));
	}
	else
	{
		mysql_push_pending(conn, MYSQL_PENDING_SYNC, NULL, NULL, NULL, 0);
	}
	return mysql_flush_out(conn);
}

static bool
mysql_pipeline_sync(PgBenchConn *conn)
{
	return mysql_send_pipeline_sync(conn);
}

/*-------------------------------------------------------------------------
 * Result Inspection
 *-------------------------------------------------------------------------
 */

static PgBenchExecStatus
mysql_result_status(const PgBenchResult *res)
{
	return res ? res->status : PGBENCH_RES_FATAL_ERROR;
}

static const char *
mysql_result_error_field(const PgBenchResult *res, int fieldcode)
{
	(void) fieldcode;
	if (res == NULL || res->sqlstate[0] == '\0')
		return NULL;
	return res->sqlstate;
}

static const char *
mysql_result_error_message(const PgBenchResult *res, const PgBenchConn *conn)
{
	if (res && res->err_msg)
		return res->err_msg;
	if (conn && conn->err_buf.data)
		return conn->err_buf.data;
	return "";
}

static int
mysql_ntuples(const PgBenchResult *res)
{
	return res ? res->nrows : 0;
}

static int
mysql_nfields(const PgBenchResult *res)
{
	return res ? res->ncols : 0;
}

static const char *
mysql_fname(const PgBenchResult *res, int col)
{
	if (res == NULL || col < 0 || col >= res->ncols || res->col_names == NULL)
		return NULL;
	return res->col_names[col];
}

static int
mysql_fnumber(const PgBenchResult *res, const char *col_name)
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
mysql_get_is_null(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->is_null == NULL)
		return true;
	return res->is_null[row][col];
}

static const char *
mysql_get_value(const PgBenchResult *res, int row, int col)
{
	if (res == NULL || row < 0 || row >= res->nrows ||
		col < 0 || col >= res->ncols || res->cells == NULL)
		return "";
	return res->cells[row][col] ? res->cells[row][col] : "";
}

static void
mysql_clear_result(PgBenchResult *res)
{
	mysql_free_result(res);
}

/*-------------------------------------------------------------------------
 * Bulk Loading (copy_begin, copy_put_data, copy_put_row, copy_end)
 *-------------------------------------------------------------------------
 */

static void
mysql_reset_copy_sql_header(PgBenchCopyStream *stream)
{
	resetPQExpBuffer(&stream->sql_buf);
	appendPQExpBuffer(&stream->sql_buf, "INSERT INTO %s", stream->table_name);
	if (stream->ncols > 0 && stream->col_names != NULL)
	{
		appendPQExpBufferStr(&stream->sql_buf, " (");
		for (int i = 0; i < stream->ncols; i++)
		{
			if (i > 0)
				appendPQExpBufferStr(&stream->sql_buf, ", ");
			appendPQExpBufferStr(&stream->sql_buf, stream->col_names[i]);
		}
		appendPQExpBufferChar(&stream->sql_buf, ')');
	}
	appendPQExpBufferStr(&stream->sql_buf, " VALUES ");
	stream->buffered_rows = 0;
}

static bool
mysql_flush_copy_batch(PgBenchCopyStream *stream)
{
	PgBenchResult *res;
	bool		ok;

	if (stream->buffered_rows == 0)
		return true;

	res = mysql_exec_sync(stream->conn, stream->sql_buf.data);
	ok = (res != NULL && res->status == PGBENCH_RES_COMMAND_OK);
	mysql_free_result(res);
	mysql_reset_copy_sql_header(stream);
	if (!ok)
		stream->failed = true;
	return ok;
}

static PgBenchCopyStream *
mysql_copy_begin(PgBenchConn *conn, const char *table_name,
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
	stream->ncols = ncols;
	if (ncols > 0 && col_names != NULL)
	{
		stream->col_names = pg_malloc0_array(char *, ncols);
		for (int i = 0; i < ncols; i++)
			stream->col_names[i] = pg_strdup(col_names[i]);
	}

	initPQExpBuffer(&stream->line_buf);
	initPQExpBuffer(&stream->sql_buf);
	mysql_reset_copy_sql_header(stream);
	return stream;
}

static void
mysql_append_copy_text_line(PgBenchCopyStream *stream, const char *line, size_t len)
{
	const char *p = line;
	const char *end = line + len;
	int			col_idx = 0;

	if (len > 0 && end[-1] == '\r')
		end--;
	if (p == end)
		return;

	if (stream->buffered_rows > 0)
		appendPQExpBufferStr(&stream->sql_buf, ", ");
	appendPQExpBufferChar(&stream->sql_buf, '(');

	for (;;)
	{
		const char *tab = memchr(p, '\t', end - p);
		const char *fend = tab ? tab : end;
		size_t		flen = (size_t) (fend - p);

		if (col_idx > 0)
			appendPQExpBufferStr(&stream->sql_buf, ", ");

		if (flen == 2 && p[0] == '\\' && p[1] == 'N')
		{
			appendPQExpBufferStr(&stream->sql_buf, "NULL");
		}
		else
		{
			char	   *val = pnstrdup(p, flen);

			mysql_append_escaped_literal(&stream->sql_buf, val);
			pg_free(val);
		}
		col_idx++;
		if (tab == NULL)
			break;
		p = tab + 1;
	}

	appendPQExpBufferChar(&stream->sql_buf, ')');
	stream->buffered_rows++;
}

static bool
mysql_copy_put_data(PgBenchCopyStream *stream, const char *buffer, int nbytes)
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
		mysql_append_copy_text_line(stream, p, (size_t) (nl - p));
		p = nl + 1;
		if (stream->buffered_rows >= 256 || stream->sql_buf.len >= 262144)
		{
			if (!mysql_flush_copy_batch(stream))
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
mysql_copy_put_row(PgBenchCopyStream *stream, int ncols,
				   const struct PgBenchValue *col_values)
{
	if (stream == NULL || stream->failed || col_values == NULL || ncols <= 0)
		return false;

	if (stream->buffered_rows > 0)
		appendPQExpBufferStr(&stream->sql_buf, ", ");
	appendPQExpBufferChar(&stream->sql_buf, '(');

	for (int c = 0; c < ncols; c++)
	{
		const PgBenchValue *v = &col_values[c];

		if (c > 0)
			appendPQExpBufferStr(&stream->sql_buf, ", ");
		switch (v->type)
		{
			case PGBT_NO_VALUE:
			case PGBT_NULL:
				appendPQExpBufferStr(&stream->sql_buf, "NULL");
				break;
			case PGBT_INT:
				appendPQExpBuffer(&stream->sql_buf, INT64_FORMAT, v->u.ival);
				break;
			case PGBT_DOUBLE:
				appendPQExpBuffer(&stream->sql_buf, "%.15g", v->u.dval);
				break;
			case PGBT_BOOLEAN:
				appendPQExpBufferStr(&stream->sql_buf, v->u.bval ? "1" : "0");
				break;
			case PGBT_TEXT:
				mysql_append_escaped_literal(&stream->sql_buf,
											 v->u.sval ? v->u.sval : "");
				break;
			case PGBT_BYTEA:
				appendPQExpBufferStr(&stream->sql_buf, "X'");
				for (int i = 0; i < v->u.byval.len; i++)
					appendPQExpBuffer(&stream->sql_buf, "%02x", v->u.byval.data[i]);
				appendPQExpBufferChar(&stream->sql_buf, '\'');
				break;
			default:
				appendPQExpBufferStr(&stream->sql_buf, "NULL");
				break;
		}
	}

	appendPQExpBufferChar(&stream->sql_buf, ')');
	stream->buffered_rows++;

	if (stream->buffered_rows >= 256 || stream->sql_buf.len >= 262144)
	{
		if (!mysql_flush_copy_batch(stream))
			return false;
	}
	return true;
}

static bool
mysql_copy_end(PgBenchCopyStream *stream, const char *errormsg)
{
	bool		ok = true;

	if (stream == NULL)
		return false;

	if (errormsg == NULL && !stream->failed)
	{
		if (stream->line_buf.len > 0)
		{
			mysql_append_copy_text_line(stream, stream->line_buf.data,
										stream->line_buf.len);
			resetPQExpBuffer(&stream->line_buf);
		}
		ok = mysql_flush_copy_batch(stream);
	}
	else
	{
		ok = false;
	}

	termPQExpBuffer(&stream->line_buf);
	termPQExpBuffer(&stream->sql_buf);
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
 * Exported MySQL Driver Vtable
 *-------------------------------------------------------------------------
 */

const PgBenchDbDriver mysql_driver = {
	.name = "mysql",
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
	.init_global = mysql_init_global,
	.shutdown_global = mysql_shutdown_global,
	.connect_sync = mysql_connect_sync,
	.connect_start = mysql_connect_start,
	.connect_poll = mysql_connect_poll,
	.finish = mysql_finish,
	.status = mysql_status,
	.error_message = mysql_error_message,
	.get_poll_fd = mysql_get_poll_fd,
	.tx_status = mysql_tx_status,
	.get_host = mysql_get_host,
	.get_port = mysql_get_port,
	.get_db = mysql_get_db,
	.set_cancel_conn = mysql_set_cancel_conn,
	.reset_cancel_conn = mysql_reset_cancel_conn,
	.exec_sync = mysql_exec_sync,
	.prepare_sync = mysql_prepare_sync,
	.escape_identifier = mysql_escape_identifier,
	.free_mem = mysql_free_mem,
	.send_query = mysql_send_query,
	.send_query_params = mysql_send_query_params,
	.send_prepare = mysql_send_prepare,
	.send_query_prepared = mysql_send_query_prepared,
	.consume_input = mysql_consume_input,
	.is_busy = mysql_is_busy,
	.get_result = mysql_get_result,
	.enter_pipeline_mode = mysql_enter_pipeline_mode,
	.exit_pipeline_mode = mysql_exit_pipeline_mode,
	.pipeline_status = mysql_pipeline_status,
	.pipeline_sync = mysql_pipeline_sync,
	.send_pipeline_sync = mysql_send_pipeline_sync,
	.result_status = mysql_result_status,
	.result_error_field = mysql_result_error_field,
	.result_error_message = mysql_result_error_message,
	.ntuples = mysql_ntuples,
	.nfields = mysql_nfields,
	.fname = mysql_fname,
	.fnumber = mysql_fnumber,
	.get_is_null = mysql_get_is_null,
	.get_value = mysql_get_value,
	.clear_result = mysql_clear_result,
	.copy_begin = mysql_copy_begin,
	.copy_put_data = mysql_copy_put_data,
	.copy_put_row = mysql_copy_put_row,
	.copy_end = mysql_copy_end
};
