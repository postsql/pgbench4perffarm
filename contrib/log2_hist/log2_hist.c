/*-------------------------------------------------------------------------
 *
 * log2_hist.c
 *	  64-bucket RLE-compressed log2(V+1) histogram data type for PostgreSQL.
 *
 *	  Properties:
 *	  - Logical model: uint64[64] counters indexed over 0-based buckets 0..63.
 *	  - Bucket mapping: bucket(V) = 64 - clz64(V) (0 -> 0, 1 -> 1, 2..3 -> 2,
 *	    4..7 -> 3, ..., 2^62..2^63-1 -> 63).
 *	  - Negative values: controlled by a 2nd boolean argument (clamp_negative)
 *	    or session GUC `log2_hist.clamp_negative` (false raises ERROR, true
 *	    clamps to bucket 0).
 *	  - Binary storage: run-length encoded varlena skipping empty buckets.
 *	  - Text representation: '[(nr-of-zeros){count[,count]*}]'.
 *	  - Subscripting: 0-based h[0..63] element fetch/assignment and h[lo:hi]
 *	    slice fetch.
 *	  - Array casts: h::bigint[] returns a 1-D array with 0-based lower bound
 *	    spanning the live [b_first:b_last] bucket range (or '{}' when empty);
 *	    bigint[]::log2_hist compresses 1-D arrays of length <= 64.
 *	  - Overflow handling: saturating unsigned 64-bit addition (branchless
 *	    scalar / AVX-512 SIMD clamping to UINT64_MAX); raw 64-bit reinterpretation
 *	    when cast to signed bigint[].
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/log2_hist/log2_hist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#if defined(__AVX512F__) && defined(__AVX512VL__)
#include <immintrin.h>
#endif

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/execExpr.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

#ifndef TYPALIGN_DOUBLE
#define TYPALIGN_DOUBLE 'd'
#endif

#define LOG2_HIST_NUM_BUCKETS	64
#define LOG2_HIST_MAX_RUNS		32

/*
 * Run descriptor: skips `skip_zeros` empty buckets (0..63), then stores
 * `run_len` contiguous non-zero uint64 counters (1..64).
 */
typedef struct Log2HistRun
{
	uint8		skip_zeros;
	uint8		run_len;
} Log2HistRun;

/*
 * RLE-compressed varlena representation of a 64-bucket log2_hist.
 *
 * Memory layout:
 *   - int32       vl_len_
 *   - uint8       num_runs   (0..32)
 *   - uint8       num_values (0..64)
 *   - uint16      flags      (0)
 *   - Log2HistRun runs[num_runs]
 *   - 0..6 bytes padding to MAXALIGN (8-byte boundary)
 *   - uint64      counts[num_values]
 */
typedef struct Log2Hist
{
	int32		vl_len_;
	uint8		num_runs;
	uint8		num_values;
	uint16		flags;
} Log2Hist;

#define LOG2_HIST_RUNS(h) \
	((Log2HistRun *) ((char *) (h) + sizeof(Log2Hist)))

#define LOG2_HIST_COUNTS_OFFSET(num_runs) \
	MAXALIGN(sizeof(Log2Hist) + (size_t) (num_runs) * sizeof(Log2HistRun))

#define LOG2_HIST_COUNTS(h) \
	((uint64 *) ((char *) (h) + LOG2_HIST_COUNTS_OFFSET((h)->num_runs)))

#define LOG2_HIST_SIZE(num_runs, num_values) \
	(LOG2_HIST_COUNTS_OFFSET(num_runs) + (size_t) (num_values) * sizeof(uint64))

#define DatumGetLog2HistP(X)	((Log2Hist *) PG_DETOAST_DATUM(X))
#define PG_GETARG_LOG2_HIST_P(n) DatumGetLog2HistP(PG_GETARG_DATUM(n))
#define PG_RETURN_LOG2_HIST_P(x) PG_RETURN_POINTER(x)

/* Flat 512-byte transition state for parallel-safe aggregates */
typedef struct Log2HistAggState
{
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
} Log2HistAggState;

/* GUC controlling default behavior on negative values when 2nd arg omitted */
static bool log2_hist_clamp_negative_guc = false;

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomBoolVariable("log2_hist.clamp_negative",
							 "Clamp negative observations to bucket 0 instead of raising an error.",
							 NULL,
							 &log2_hist_clamp_negative_guc,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("log2_hist");
#else
	EmitWarningsOnPlaceholders("log2_hist");
#endif
}

/* -------------------------------------------------------------------------
 * Internal Helper Functions
 * -------------------------------------------------------------------------
 */

/*
 * Alignment-safe 64-bit unsigned counter read.
 */
static inline uint64
log2_hist_read_u64(const void *base, int idx)
{
	uint64		val;

	memcpy(&val, (const char *) base + (size_t) idx * sizeof(uint64), sizeof(uint64));
	return val;
}

/*
 * Saturating unsigned 64-bit scalar addition (clamps at UINT64_MAX).
 * Compiles to branchless ADD + CMOVC on x86-64 / ADDS + CSINV on ARM64.
 */
static inline uint64
sat_add_u64(uint64 a, uint64 b)
{
	uint64		res;

	return __builtin_add_overflow(a, b, &res) ? PG_UINT64_MAX : res;
}

/*
 * Pointwise saturating unsigned 64-bit addition across all 64 buckets.
 * Uses AVX-512 intrinsics when compiled with -mavx512f -mavx512vl, or
 * auto-vectorized / branchless scalar overflow intrinsics otherwise.
 */
static inline void
log2_hist_add_buckets_sat(uint64 dst[LOG2_HIST_NUM_BUCKETS],
						  const uint64 src[LOG2_HIST_NUM_BUCKETS])
{
#if defined(__AVX512F__) && defined(__AVX512VL__)
	const __m512i all_ones = _mm512_set1_epi64(-1LL);

	for (int i = 0; i < LOG2_HIST_NUM_BUCKETS; i += 8)
	{
		__m512i		va = _mm512_loadu_si512((const __m512i *) &dst[i]);
		__m512i		vb = _mm512_loadu_si512((const __m512i *) &src[i]);
		__m512i		vsum = _mm512_add_epi64(va, vb);
		__mmask8	ovf = _mm512_cmp_epu64_mask(vsum, va, _MM_CMPINT_LT);
		__m512i		vsat = _mm512_mask_mov_epi64(vsum, ovf, all_ones);

		_mm512_storeu_si512((__m512i *) &dst[i], vsat);
	}
#else
	for (int i = 0; i < LOG2_HIST_NUM_BUCKETS; i++)
	{
		uint64		sum;

		dst[i] = __builtin_add_overflow(dst[i], src[i], &sum) ? PG_UINT64_MAX : sum;
	}
#endif
}

/*
 * Compute bucket index (0..63) for observation v:
 *   v == 0 -> 0
 *   v > 0  -> 64 - clz64(v) = pg_leftmost_one_pos64((uint64) v) + 1
 *   v < 0  -> 0 if clamp_negative, else ERROR
 */
static inline int
log2_hist_bucket_index(int64 v, bool clamp_negative)
{
	if (unlikely(v <= 0))
	{
		if (v < 0 && !clamp_negative)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("log2_hist value cannot be negative: " INT64_FORMAT, v),
					 errhint("Pass clamp_negative = true or SET log2_hist.clamp_negative = on to clamp negative values to bucket 0.")));
		return 0;
	}

	return pg_leftmost_one_pos64((uint64) v) + 1;
}

/*
 * Compress a 64-element uint64 array into canonical RLE Log2Hist varlena.
 */
static Log2Hist *
log2_hist_compress(const uint64 buckets[LOG2_HIST_NUM_BUCKETS])
{
	Log2HistRun runs[LOG2_HIST_MAX_RUNS];
	uint8		num_runs = 0;
	uint8		num_values = 0;
	uint8		skip = 0;
	int			i = 0;
	size_t		total_size;
	Log2Hist   *h;

	while (i < LOG2_HIST_NUM_BUCKETS)
	{
		if (buckets[i] == 0)
		{
			skip++;
			i++;
		}
		else
		{
			uint8		run_len = 0;

			while (i < LOG2_HIST_NUM_BUCKETS && buckets[i] != 0)
			{
				run_len++;
				i++;
			}
			runs[num_runs].skip_zeros = skip;
			runs[num_runs].run_len = run_len;
			num_runs++;
			num_values += run_len;
			skip = 0;
		}
	}

	total_size = LOG2_HIST_SIZE(num_runs, num_values);
	/* palloc0 ensures alignment padding bytes between runs and counts are 0 */
	h = (Log2Hist *) palloc0(total_size);
	SET_VARSIZE(h, total_size);
	h->num_runs = num_runs;
	h->num_values = num_values;
	h->flags = 0;

	if (num_runs > 0)
	{
		uint64	   *out_counts = LOG2_HIST_COUNTS(h);
		int			val_idx = 0;

		memcpy(LOG2_HIST_RUNS(h), runs, num_runs * sizeof(Log2HistRun));
		for (i = 0; i < LOG2_HIST_NUM_BUCKETS; i++)
		{
			if (buckets[i] != 0)
				out_counts[val_idx++] = buckets[i];
		}
	}

	return h;
}

/*
 * Decompress a Log2Hist varlena into a 64-element uint64 array.
 * Optionally outputs the first and last non-zero bucket indices (0..63).
 */
static void
log2_hist_decompress(const Log2Hist *h,
					 uint64 buckets[LOG2_HIST_NUM_BUCKETS],
					 int *first_nonzero,
					 int *last_nonzero)
{
	const Log2HistRun *runs;
	const void *counts;
	int			bucket_idx = 0;
	int			val_idx = 0;
	int			b_first = -1;
	int			b_last = -1;

	memset(buckets, 0, LOG2_HIST_NUM_BUCKETS * sizeof(uint64));

	if (unlikely(VARSIZE(h) < sizeof(Log2Hist) ||
				 h->num_runs > LOG2_HIST_MAX_RUNS ||
				 h->num_values > LOG2_HIST_NUM_BUCKETS ||
				 VARSIZE(h) < LOG2_HIST_SIZE(h->num_runs, h->num_values)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted log2_hist header")));

	runs = LOG2_HIST_RUNS(h);
	counts = (const void *) LOG2_HIST_COUNTS(h);

	for (int r = 0; r < h->num_runs; r++)
	{
		uint8		skip = runs[r].skip_zeros;
		uint8		len = runs[r].run_len;

		bucket_idx += skip;
		if (unlikely(len == 0 ||
					 bucket_idx + len > LOG2_HIST_NUM_BUCKETS ||
					 val_idx + len > h->num_values))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted log2_hist RLE run descriptor")));

		if (b_first < 0)
			b_first = bucket_idx;
		b_last = bucket_idx + len - 1;

		memcpy(&buckets[bucket_idx],
			   (const char *) counts + (size_t) val_idx * sizeof(uint64),
			   len * sizeof(uint64));
		bucket_idx += len;
		val_idx += len;
	}

	if (unlikely(val_idx != h->num_values))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted log2_hist value count")));

	if (first_nonzero)
		*first_nonzero = b_first;
	if (last_nonzero)
		*last_nonzero = b_last;
}

/*
 * Fast O(num_runs) single-bucket lookup without full decompression.
 * Caller must ensure 0 <= target_bucket < 64.
 */
static uint64
log2_hist_get_bucket_fast(const Log2Hist *h, int target_bucket)
{
	const Log2HistRun *runs = LOG2_HIST_RUNS(h);
	const void *counts = (const void *) LOG2_HIST_COUNTS(h);
	int			bucket_idx = 0;
	int			val_idx = 0;

	for (int r = 0; r < h->num_runs; r++)
	{
		uint8		skip = runs[r].skip_zeros;
		uint8		len = runs[r].run_len;

		bucket_idx += skip;
		if (target_bucket < bucket_idx)
			return 0;
		if (target_bucket < bucket_idx + len)
			return log2_hist_read_u64(counts, val_idx + (target_bucket - bucket_idx));

		bucket_idx += len;
		val_idx += len;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Textual Input / Output and Binary Send / Receive
 * -------------------------------------------------------------------------
 */

/*
 * Parse textual format: '[(nr-of-zeros){count[,count]*}]'
 */
PG_FUNCTION_INFO_V1(log2_hist_in);
Datum
log2_hist_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	const char *p = str;
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
	int			bucket_idx = 0;

	memset(buckets, 0, sizeof(buckets));

	while (isspace((unsigned char) *p))
		p++;

	if (*p != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
				 errdetail("Histogram representation must begin with '['.")));
	p++;

	while (isspace((unsigned char) *p))
		p++;

	while (*p != '\0' && *p != ']')
	{
		char	   *endptr;
		unsigned long long zero_count;

		if (*p != '(')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
					 errdetail("Expected '(' to start zero-bucket count.")));
		p++;

		while (isspace((unsigned char) *p))
			p++;

		if (!isdigit((unsigned char) *p))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
					 errdetail("Expected non-negative integer inside '(...)'.")));

		errno = 0;
		zero_count = strtoull(p, &endptr, 10);
		if (errno == ERANGE || endptr == p)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("zero-bucket count out of range in log2_hist: \"%s\"", str)));
		p = endptr;

		while (isspace((unsigned char) *p))
			p++;

		if (*p != ')')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
					 errdetail("Expected ')' after zero-bucket count.")));
		p++;

		if (zero_count >= LOG2_HIST_NUM_BUCKETS ||
			bucket_idx + (int) zero_count >= LOG2_HIST_NUM_BUCKETS)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("log2_hist bucket index out of bounds (0..63) in \"%s\"", str)));

		bucket_idx += (int) zero_count;

		while (isspace((unsigned char) *p))
			p++;

		if (*p != '{')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
					 errdetail("Expected '{' after '(nr-of-zeros)'.")));
		p++;

		/* Parse one or more comma-separated uint64 counts inside {...} */
		for (;;)
		{
			unsigned long long cnt;

			while (isspace((unsigned char) *p))
				p++;

			if (!isdigit((unsigned char) *p))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
						 errdetail("Expected unsigned integer bucket count inside '{...}'.")));

			errno = 0;
			cnt = strtoull(p, &endptr, 10);
			if (errno == ERANGE || endptr == p)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bucket count out of uint64 range in log2_hist: \"%s\"", str)));
			p = endptr;

			if (bucket_idx >= LOG2_HIST_NUM_BUCKETS)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("log2_hist bucket index exceeds maximum bucket 63 in \"%s\"", str)));

			buckets[bucket_idx++] = (uint64) cnt;

			while (isspace((unsigned char) *p))
				p++;

			if (*p == ',')
			{
				p++;
				continue;
			}
			else if (*p == '}')
			{
				p++;
				break;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
						 errdetail("Expected ',' or '}' in bucket count list.")));
		}

		while (isspace((unsigned char) *p))
			p++;
	}

	if (*p != ']')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
				 errdetail("Expected closing ']'.")));
	p++;

	while (isspace((unsigned char) *p))
		p++;

	if (*p != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type log2_hist: \"%s\"", str),
				 errdetail("Unexpected trailing characters after ']'.")));

	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

/*
 * Format canonical text output: '[(nr-of-zeros){count[,count]*}]'
 */
PG_FUNCTION_INFO_V1(log2_hist_out);
Datum
log2_hist_out(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	const Log2HistRun *runs = LOG2_HIST_RUNS(h);
	const void *counts = (const void *) LOG2_HIST_COUNTS(h);
	StringInfoData buf;
	int			val_idx = 0;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');

	for (int r = 0; r < h->num_runs; r++)
	{
		appendStringInfo(&buf, "(%u){", (unsigned int) runs[r].skip_zeros);
		for (int k = 0; k < runs[r].run_len; k++)
		{
			if (k > 0)
				appendStringInfoChar(&buf, ',');
			appendStringInfo(&buf, UINT64_FORMAT, log2_hist_read_u64(counts, val_idx++));
		}
		appendStringInfoChar(&buf, '}');
	}

	appendStringInfoChar(&buf, ']');
	PG_RETURN_CSTRING(buf.data);
}

/*
 * Binary wire receive function.
 */
PG_FUNCTION_INFO_V1(log2_hist_recv);
Datum
log2_hist_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint8		num_runs;
	uint8		num_values;
	uint16		flags;
	Log2HistRun runs[LOG2_HIST_MAX_RUNS];
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
	int			bucket_idx = 0;
	int			val_idx = 0;

	num_runs = pq_getmsgbyte(buf);
	num_values = pq_getmsgbyte(buf);
	flags = pq_getmsgint(buf, sizeof(uint16));

	if (num_runs > LOG2_HIST_MAX_RUNS ||
		num_values > LOG2_HIST_NUM_BUCKETS ||
		flags != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid binary header for log2_hist")));

	for (int r = 0; r < num_runs; r++)
	{
		runs[r].skip_zeros = pq_getmsgbyte(buf);
		runs[r].run_len = pq_getmsgbyte(buf);
	}

	memset(buckets, 0, sizeof(buckets));

	for (int r = 0; r < num_runs; r++)
	{
		uint8		skip = runs[r].skip_zeros;
		uint8		len = runs[r].run_len;

		bucket_idx += skip;
		if (len == 0 ||
			bucket_idx + len > LOG2_HIST_NUM_BUCKETS ||
			val_idx + len > num_values)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid RLE run in binary log2_hist")));

		for (int k = 0; k < len; k++)
		{
			buckets[bucket_idx++] = (uint64) pq_getmsgint64(buf);
			val_idx++;
		}
	}

	if (val_idx != num_values)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("mismatched value count in binary log2_hist")));

	pq_getmsgend(buf);

	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

/*
 * Binary wire send function.
 */
PG_FUNCTION_INFO_V1(log2_hist_send);
Datum
log2_hist_send(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	const Log2HistRun *runs = LOG2_HIST_RUNS(h);
	const void *counts = (const void *) LOG2_HIST_COUNTS(h);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, h->num_runs);
	pq_sendbyte(&buf, h->num_values);
	pq_sendint16(&buf, h->flags);

	for (int r = 0; r < h->num_runs; r++)
	{
		pq_sendbyte(&buf, runs[r].skip_zeros);
		pq_sendbyte(&buf, runs[r].run_len);
	}

	for (int v = 0; v < h->num_values; v++)
		pq_sendint64(&buf, (int64) log2_hist_read_u64(counts, v));

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Deserialize binary wire representation from bytea via log2_hist_recv.
 */
PG_FUNCTION_INFO_V1(log2_hist_from_bytea);
Datum
log2_hist_from_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	StringInfoData buf;

	buf.data = VARDATA_ANY(data);
	buf.len = VARSIZE_ANY_EXHDR(data);
	buf.maxlen = buf.len;
	buf.cursor = 0;

	return DirectFunctionCall1(log2_hist_recv, PointerGetDatum(&buf));
}

/* -------------------------------------------------------------------------
 * Raw Subscripting Support (0-based 0..63 indexing & slicing)
 * -------------------------------------------------------------------------
 */

static void
log2_hist_subscript_transform(SubscriptingRef *sbsref,
							  List *indirection,
							  ParseState *pstate,
							  bool isSlice,
							  bool isAssignment)
{
	A_Indices  *ai;
	Node	   *subexpr;

	if (list_length(indirection) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("log2_hist allows only one subscript"),
				 parser_errposition(pstate,
									exprLocation((Node *) indirection))));

	if (isSlice && isAssignment)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("log2_hist slice assignment is not supported"),
				 parser_errposition(pstate,
									exprLocation((Node *) indirection))));

	ai = linitial_node(A_Indices, indirection);

	if (isSlice)
	{
		Node	   *lowexpr = NULL;
		Node	   *upexpr = NULL;

		if (ai->lidx != NULL)
		{
			lowexpr = transformExpr(pstate, ai->lidx, pstate->p_expr_kind);
			lowexpr = coerce_to_target_type(pstate,
											lowexpr, exprType(lowexpr),
											INT4OID, -1,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
			if (lowexpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("log2_hist subscript must have type integer"),
						 parser_errposition(pstate, exprLocation(ai->lidx))));
		}

		if (ai->uidx != NULL)
		{
			upexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			upexpr = coerce_to_target_type(pstate,
										   upexpr, exprType(upexpr),
										   INT4OID, -1,
										   COERCION_ASSIGNMENT,
										   COERCE_IMPLICIT_CAST,
										   -1);
			if (upexpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("log2_hist subscript must have type integer"),
						 parser_errposition(pstate, exprLocation(ai->uidx))));
		}

		sbsref->reflowerindexpr = list_make1(lowexpr);
		sbsref->refupperindexpr = list_make1(upexpr);
		sbsref->refrestype = sbsref->refcontainertype;
		sbsref->reftypmod = -1;
	}
	else
	{
		Assert(ai->uidx != NULL && ai->lidx == NULL && !ai->is_slice);

		subexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
		subexpr = coerce_to_target_type(pstate,
										subexpr, exprType(subexpr),
										INT4OID, -1,
										COERCION_ASSIGNMENT,
										COERCE_IMPLICIT_CAST,
										-1);
		if (subexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("log2_hist subscript must have type integer"),
					 parser_errposition(pstate, exprLocation(ai->uidx))));

		sbsref->refupperindexpr = list_make1(subexpr);
		sbsref->reflowerindexpr = NIL;
		sbsref->refrestype = INT8OID;
		sbsref->reftypmod = -1;
	}
}

static void
log2_hist_subscript_fetch(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	Log2Hist   *h;
	int32		idx;

	Assert(!(*op->resnull));

	if (sbsrefstate->upperindexnull[0])
	{
		*op->resnull = true;
		return;
	}

	idx = DatumGetInt32(sbsrefstate->upperindex[0]);
	if (idx < 0 || idx >= LOG2_HIST_NUM_BUCKETS)
	{
		*op->resnull = true;
		return;
	}

	h = DatumGetLog2HistP(*op->resvalue);
	*op->resvalue = Int64GetDatum((int64) log2_hist_get_bucket_fast(h, idx));
}

static void
log2_hist_subscript_fetch_slice(ExprState *state,
								ExprEvalStep *op,
								ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	Log2Hist   *h;
	int32		lo;
	int32		hi;
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	Assert(!(*op->resnull));

	if ((sbsrefstate->lowerprovided[0] && sbsrefstate->lowerindexnull[0]) ||
		(sbsrefstate->upperprovided[0] && sbsrefstate->upperindexnull[0]))
	{
		*op->resnull = true;
		return;
	}

	lo = sbsrefstate->lowerprovided[0] ?
		DatumGetInt32(sbsrefstate->lowerindex[0]) : 0;
	hi = sbsrefstate->upperprovided[0] ?
		DatumGetInt32(sbsrefstate->upperindex[0]) : (LOG2_HIST_NUM_BUCKETS - 1);

	h = DatumGetLog2HistP(*op->resvalue);
	log2_hist_decompress(h, buckets, NULL, NULL);

	for (int i = 0; i < LOG2_HIST_NUM_BUCKETS; i++)
	{
		if (i < lo || i > hi)
			buckets[i] = 0;
	}

	*op->resvalue = PointerGetDatum(log2_hist_compress(buckets));
}

static void
log2_hist_subscript_assign(ExprState *state,
						   ExprEvalStep *op,
						   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	int32		idx;
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	if (sbsrefstate->upperindexnull[0])
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("log2_hist subscript in assignment must not be null")));

	if (sbsrefstate->replacenull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("log2_hist bucket value in assignment must not be null")));

	idx = DatumGetInt32(sbsrefstate->upperindex[0]);
	if (idx < 0 || idx >= LOG2_HIST_NUM_BUCKETS)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("log2_hist subscript %d out of bounds (0..63)", idx)));

	if (*op->resnull)
		memset(buckets, 0, sizeof(buckets));
	else
		log2_hist_decompress(DatumGetLog2HistP(*op->resvalue), buckets, NULL, NULL);

	buckets[idx] = (uint64) DatumGetInt64(sbsrefstate->replacevalue);

	*op->resvalue = PointerGetDatum(log2_hist_compress(buckets));
	*op->resnull = false;
}

static void
log2_hist_exec_setup(const SubscriptingRef *sbsref,
					 SubscriptingRefState *sbsrefstate,
					 SubscriptExecSteps *methods)
{
	bool		isSlice = (sbsrefstate->numlower != 0);

	Assert(sbsrefstate->numupper == 1);
	methods->sbs_check_subscripts = NULL;
	methods->sbs_fetch = isSlice ?
		log2_hist_subscript_fetch_slice : log2_hist_subscript_fetch;
	methods->sbs_assign = isSlice ? NULL : log2_hist_subscript_assign;
	methods->sbs_fetch_old = NULL;
}

PG_FUNCTION_INFO_V1(log2_hist_subscript_handler);
Datum
log2_hist_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = log2_hist_subscript_transform,
		.exec_setup = log2_hist_exec_setup,
		.fetch_strict = true,
		.fetch_leakproof = true,
		.store_leakproof = false
	};

	PG_RETURN_POINTER(&sbsroutines);
}

/* -------------------------------------------------------------------------
 * Casts & Array Conversions (bigint[] <-> log2_hist)
 * -------------------------------------------------------------------------
 */

static ArrayType *
log2_hist_build_array(const Log2Hist *h, bool full_range, int index_base)
{
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
	Datum		elems[LOG2_HIST_NUM_BUCKETS];
	int			b_first = -1;
	int			b_last = -1;
	int			start_idx;
	int			len;
	int			dims[1];
	int			lbs[1];

	log2_hist_decompress(h, buckets, &b_first, &b_last);

	if (full_range)
	{
		start_idx = 0;
		len = LOG2_HIST_NUM_BUCKETS;
	}
	else
	{
		if (b_first < 0)
			return construct_empty_array(INT8OID);
		start_idx = b_first;
		len = b_last - b_first + 1;
	}

	for (int i = 0; i < len; i++)
		elems[i] = Int64GetDatum((int64) buckets[start_idx + i]);

	dims[0] = len;
	lbs[0] = start_idx + index_base;

	return construct_md_array(elems, NULL, 1, dims, lbs,
							  INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE);
}

/*
 * Cast log2_hist -> bigint[]: returns 0-based live-range array [b_first:b_last]
 * (or '{}' when empty).
 */
PG_FUNCTION_INFO_V1(log2_hist_to_int8array);
Datum
log2_hist_to_int8array(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);

	PG_RETURN_ARRAYTYPE_P(log2_hist_build_array(h, false, 0));
}

/*
 * Return full 64-element 0-based bigint[] ([0:63]={...}).
 */
PG_FUNCTION_INFO_V1(log2_hist_buckets);
Datum
log2_hist_buckets(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);

	PG_RETURN_ARRAYTYPE_P(log2_hist_build_array(h, true, 0));
}

/*
 * Configurable array expansion:
 *   full_range = true (default): all 64 buckets
 *   full_range = false: live non-zero bucket range only
 *   index_base = 0 (default): 0-based bounds (0..63)
 *   index_base = 1: 1-based bounds (1..64)
 */
PG_FUNCTION_INFO_V1(log2_hist_to_array);
Datum
log2_hist_to_array(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	bool		full_range = PG_GETARG_BOOL(1);
	int32		index_base = PG_GETARG_INT32(2);

	if (index_base != 0 && index_base != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("log2_hist_to_array index_base must be 0 or 1: %d",
						index_base)));

	PG_RETURN_ARRAYTYPE_P(log2_hist_build_array(h, full_range, index_base));
}

/*
 * Cast bigint[] -> log2_hist:
 * Accepts any 1-D bigint[] of length <= 64.
 * Maps array elements using 0-based lower bound `lbound` (0..63), while also
 * accepting a full 64-element 1-based SQL array (`[1:64]`, where lbound == 1
 * and nelems == 64) as buckets 0..63.
 */
PG_FUNCTION_INFO_V1(int8array_to_log2_hist);
Datum
int8array_to_log2_hist(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	int			ndim = ARR_NDIM(arr);
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
	int			nelems;
	int			lbound;
	int			base_bucket;

	memset(buckets, 0, sizeof(buckets));

	if (ndim == 0)
		PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));

	if (ndim != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("log2_hist requires a 1-dimensional bigint[] array")));

	nelems = ARR_DIMS(arr)[0];
	lbound = ARR_LBOUND(arr)[0];

	if (nelems == 0)
		PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));

	/*
	 * A full 64-element SQL array with default 1-based lower bound ([1:64])
	 * maps directly to buckets 0..63. Otherwise lbound is interpreted directly
	 * in 0-based bucket coordinates (0..63).
	 */
	if (lbound == 1 && nelems == LOG2_HIST_NUM_BUCKETS)
		base_bucket = 0;
	else
		base_bucket = lbound;

	if (base_bucket < 0 ||
		base_bucket >= LOG2_HIST_NUM_BUCKETS ||
		nelems < 0 ||
		nelems > LOG2_HIST_NUM_BUCKETS - base_bucket)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("bigint[] bounds [%d:%lld] out of range for 64-bucket log2_hist (0..63)",
						lbound, (long long) lbound + nelems - 1)));

	if (ARR_HASNULL(arr))
	{
		const uint8 *bitmap = (const uint8 *) ARR_NULLBITMAP(arr);
		const char *data = (const char *) ARR_DATA_PTR(arr);
		int			val_idx = 0;

		for (int i = 0; i < nelems; i++)
		{
			if (bitmap && !(bitmap[i / 8] & (1 << (i % 8))))
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("log2_hist array cannot contain NULL elements")));
			buckets[base_bucket + i] = log2_hist_read_u64(data, val_idx++);
		}
	}
	else
	{
		const char *data = (const char *) ARR_DATA_PTR(arr);

		memcpy(&buckets[base_bucket], data, (size_t) nelems * sizeof(uint64));
	}

	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

/* -------------------------------------------------------------------------
 * Observation, Merge, Comparison, Hash & Analytics Functions
 * -------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(log2_hist_bucket_1);
Datum
log2_hist_bucket_1(PG_FUNCTION_ARGS)
{
	int64		v = PG_GETARG_INT64(0);

	PG_RETURN_INT32(log2_hist_bucket_index(v, log2_hist_clamp_negative_guc));
}

PG_FUNCTION_INFO_V1(log2_hist_bucket_2);
Datum
log2_hist_bucket_2(PG_FUNCTION_ARGS)
{
	int64		v = PG_GETARG_INT64(0);
	bool		clamp_negative = PG_GETARG_BOOL(1);

	PG_RETURN_INT32(log2_hist_bucket_index(v, clamp_negative));
}

PG_FUNCTION_INFO_V1(log2_hist_empty);
Datum
log2_hist_empty(PG_FUNCTION_ARGS)
{
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	memset(buckets, 0, sizeof(buckets));
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_single_1);
Datum
log2_hist_single_1(PG_FUNCTION_ARGS)
{
	int64		v = PG_GETARG_INT64(0);
	int			idx = log2_hist_bucket_index(v, log2_hist_clamp_negative_guc);
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	memset(buckets, 0, sizeof(buckets));
	buckets[idx] = 1;
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_single_3);
Datum
log2_hist_single_3(PG_FUNCTION_ARGS)
{
	int64		v = PG_GETARG_INT64(0);
	bool		clamp_negative = PG_GETARG_BOOL(1);
	int64		count = PG_GETARG_INT64(2);
	int			idx;
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	if (unlikely(count < 0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("log2_hist count cannot be negative: " INT64_FORMAT, count)));

	idx = log2_hist_bucket_index(v, clamp_negative);
	memset(buckets, 0, sizeof(buckets));
	buckets[idx] = (uint64) count;
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_add_2);
Datum
log2_hist_add_2(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	int64		v = PG_GETARG_INT64(1);
	int			idx = log2_hist_bucket_index(v, log2_hist_clamp_negative_guc);
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	log2_hist_decompress(h, buckets, NULL, NULL);
	buckets[idx] = sat_add_u64(buckets[idx], 1);
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_add_rev);
Datum
log2_hist_add_rev(PG_FUNCTION_ARGS)
{
	int64		v = PG_GETARG_INT64(0);
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(1);
	int			idx = log2_hist_bucket_index(v, log2_hist_clamp_negative_guc);
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	log2_hist_decompress(h, buckets, NULL, NULL);
	buckets[idx] = sat_add_u64(buckets[idx], 1);
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_add_4);
Datum
log2_hist_add_4(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	int64		v = PG_GETARG_INT64(1);
	bool		clamp_negative = PG_GETARG_BOOL(2);
	int64		count = PG_GETARG_INT64(3);
	int			idx;
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];

	if (unlikely(count < 0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("log2_hist count cannot be negative: " INT64_FORMAT, count)));

	idx = log2_hist_bucket_index(v, clamp_negative);
	if (count == 0)
		PG_RETURN_LOG2_HIST_P(h);

	log2_hist_decompress(h, buckets, NULL, NULL);
	buckets[idx] = sat_add_u64(buckets[idx], (uint64) count);
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets));
}

PG_FUNCTION_INFO_V1(log2_hist_merge);
Datum
log2_hist_merge(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);
	uint64		buckets_a[LOG2_HIST_NUM_BUCKETS];
	uint64		buckets_b[LOG2_HIST_NUM_BUCKETS];

	if (a->num_runs == 0)
		PG_RETURN_LOG2_HIST_P(b);
	if (b->num_runs == 0)
		PG_RETURN_LOG2_HIST_P(a);

	log2_hist_decompress(a, buckets_a, NULL, NULL);
	log2_hist_decompress(b, buckets_b, NULL, NULL);
	log2_hist_add_buckets_sat(buckets_a, buckets_b);

	PG_RETURN_LOG2_HIST_P(log2_hist_compress(buckets_a));
}

PG_FUNCTION_INFO_V1(log2_hist_total_count);
Datum
log2_hist_total_count(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	const void *counts = (const void *) LOG2_HIST_COUNTS(h);
	uint64		total = 0;

	for (int i = 0; i < h->num_values; i++)
		total = sat_add_u64(total, log2_hist_read_u64(counts, i));

	PG_RETURN_INT64((int64) total);
}

PG_FUNCTION_INFO_V1(log2_hist_min_bucket);
Datum
log2_hist_min_bucket(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);

	if (h->num_runs == 0)
		PG_RETURN_NULL();

	PG_RETURN_INT32((int32) LOG2_HIST_RUNS(h)[0].skip_zeros);
}

PG_FUNCTION_INFO_V1(log2_hist_max_bucket);
Datum
log2_hist_max_bucket(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	const Log2HistRun *runs;
	int			bucket_idx = 0;

	if (h->num_runs == 0)
		PG_RETURN_NULL();

	runs = LOG2_HIST_RUNS(h);
	for (int r = 0; r < h->num_runs; r++)
		bucket_idx += (int) runs[r].skip_zeros + (int) runs[r].run_len;

	PG_RETURN_INT32(bucket_idx - 1);
}

PG_FUNCTION_INFO_V1(log2_hist_percentile);
Datum
log2_hist_percentile(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	float8		p = PG_GETARG_FLOAT8(1);
	uint64		buckets[LOG2_HIST_NUM_BUCKETS];
	int			b_first = -1;
	int			b_last = -1;
	double		total = 0.0;
	double		target;
	double		cum = 0.0;

	if (isnan(p) || p < 0.0 || p > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("log2_hist_percentile quantile must be in [0.0, 1.0]: %g", p)));

	log2_hist_decompress(h, buckets, &b_first, &b_last);
	if (b_first < 0)
		PG_RETURN_NULL();

	for (int b = b_first; b <= b_last; b++)
		total += (double) buckets[b];

	if (total <= 0.0)
		PG_RETURN_NULL();

	target = p * total;

	for (int b = b_first; b <= b_last; b++)
	{
		uint64		cnt = buckets[b];

		if (cnt == 0)
			continue;

		if (cum + (double) cnt >= target || b == b_last)
		{
			uint64		v_min;
			uint64		width;
			double		frac;
			uint64		est;

			if (b == 0)
				PG_RETURN_INT64(0);
			if (b == 1)
				PG_RETURN_INT64(1);

			v_min = 1ULL << (b - 1);
			width = (1ULL << (b - 1)) - 1ULL;
			frac = (target - cum) / (double) cnt;
			if (frac < 0.0)
				frac = 0.0;
			else if (frac > 1.0)
				frac = 1.0;

			est = v_min + (uint64) llround(frac * (double) width);
			if (est > (uint64) PG_INT64_MAX)
				est = (uint64) PG_INT64_MAX;
			PG_RETURN_INT64((int64) est);
		}

		cum += (double) cnt;
	}

	PG_RETURN_NULL();
}

static int
log2_hist_cmp_internal(const Log2Hist *a, const Log2Hist *b)
{
	uint64		buckets_a[LOG2_HIST_NUM_BUCKETS];
	uint64		buckets_b[LOG2_HIST_NUM_BUCKETS];

	if (VARSIZE(a) == VARSIZE(b) &&
		memcmp(VARDATA(a), VARDATA(b), VARSIZE(a) - VARHDRSZ) == 0)
		return 0;

	log2_hist_decompress(a, buckets_a, NULL, NULL);
	log2_hist_decompress(b, buckets_b, NULL, NULL);

	for (int i = 0; i < LOG2_HIST_NUM_BUCKETS; i++)
	{
		if (buckets_a[i] < buckets_b[i])
			return -1;
		if (buckets_a[i] > buckets_b[i])
			return 1;
	}

	return 0;
}

PG_FUNCTION_INFO_V1(log2_hist_eq);
Datum
log2_hist_eq(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);
	bool		eq;

	if (VARSIZE(a) != VARSIZE(b))
		eq = false;
	else
		eq = (memcmp(VARDATA(a), VARDATA(b), VARSIZE(a) - VARHDRSZ) == 0);

	PG_RETURN_BOOL(eq);
}

PG_FUNCTION_INFO_V1(log2_hist_ne);
Datum
log2_hist_ne(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);
	bool		ne;

	if (VARSIZE(a) != VARSIZE(b))
		ne = true;
	else
		ne = (memcmp(VARDATA(a), VARDATA(b), VARSIZE(a) - VARHDRSZ) != 0);

	PG_RETURN_BOOL(ne);
}

PG_FUNCTION_INFO_V1(log2_hist_lt);
Datum
log2_hist_lt(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);

	PG_RETURN_BOOL(log2_hist_cmp_internal(a, b) < 0);
}

PG_FUNCTION_INFO_V1(log2_hist_le);
Datum
log2_hist_le(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);

	PG_RETURN_BOOL(log2_hist_cmp_internal(a, b) <= 0);
}

PG_FUNCTION_INFO_V1(log2_hist_ge);
Datum
log2_hist_ge(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);

	PG_RETURN_BOOL(log2_hist_cmp_internal(a, b) >= 0);
}

PG_FUNCTION_INFO_V1(log2_hist_gt);
Datum
log2_hist_gt(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);

	PG_RETURN_BOOL(log2_hist_cmp_internal(a, b) > 0);
}

PG_FUNCTION_INFO_V1(log2_hist_cmp);
Datum
log2_hist_cmp(PG_FUNCTION_ARGS)
{
	Log2Hist   *a = PG_GETARG_LOG2_HIST_P(0);
	Log2Hist   *b = PG_GETARG_LOG2_HIST_P(1);

	PG_RETURN_INT32(log2_hist_cmp_internal(a, b));
}

PG_FUNCTION_INFO_V1(log2_hist_hash);
Datum
log2_hist_hash(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);

	return hash_any((const unsigned char *) VARDATA(h),
					VARSIZE(h) - VARHDRSZ);
}

PG_FUNCTION_INFO_V1(log2_hist_hash_extended);
Datum
log2_hist_hash_extended(PG_FUNCTION_ARGS)
{
	Log2Hist   *h = PG_GETARG_LOG2_HIST_P(0);
	uint64		seed = (uint64) PG_GETARG_INT64(1);

	return hash_any_extended((const unsigned char *) VARDATA(h),
							 VARSIZE(h) - VARHDRSZ,
							 seed);
}

/* -------------------------------------------------------------------------
 * Parallel-Safe Aggregate Support Functions
 * -------------------------------------------------------------------------
 */

static Log2HistAggState *
log2_hist_init_agg_state(FunctionCallInfo fcinfo)
{
	MemoryContext aggcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("log2_hist aggregate transition function called in non-aggregate context")));

	return (Log2HistAggState *) MemoryContextAllocZero(aggcontext,
													   sizeof(Log2HistAggState));
}

PG_FUNCTION_INFO_V1(log2_hist_agg_transfn_1);
Datum
log2_hist_agg_transfn_1(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state;
	int			idx;

	state = PG_ARGISNULL(0) ? NULL : (Log2HistAggState *) PG_GETARG_POINTER(0);

	if (PG_ARGISNULL(1))
	{
		if (state == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state);
	}

	idx = log2_hist_bucket_index(PG_GETARG_INT64(1), log2_hist_clamp_negative_guc);
	if (state == NULL)
		state = log2_hist_init_agg_state(fcinfo);

	state->buckets[idx] = sat_add_u64(state->buckets[idx], 1);
	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(log2_hist_agg_transfn_2);
Datum
log2_hist_agg_transfn_2(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state;
	bool		clamp_negative;
	int			idx;

	state = PG_ARGISNULL(0) ? NULL : (Log2HistAggState *) PG_GETARG_POINTER(0);

	if (PG_ARGISNULL(1))
	{
		if (state == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state);
	}

	clamp_negative = PG_ARGISNULL(2) ?
		log2_hist_clamp_negative_guc : PG_GETARG_BOOL(2);
	idx = log2_hist_bucket_index(PG_GETARG_INT64(1), clamp_negative);

	if (state == NULL)
		state = log2_hist_init_agg_state(fcinfo);

	state->buckets[idx] = sat_add_u64(state->buckets[idx], 1);
	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(log2_hist_merge_agg_transfn);
Datum
log2_hist_merge_agg_transfn(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state;
	Log2Hist   *h;
	const Log2HistRun *runs;
	const void *counts;
	int			b_idx = 0;
	int			v_idx = 0;

	state = PG_ARGISNULL(0) ? NULL : (Log2HistAggState *) PG_GETARG_POINTER(0);

	if (PG_ARGISNULL(1))
	{
		if (state == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state);
	}

	h = PG_GETARG_LOG2_HIST_P(1);
	if (state == NULL)
		state = log2_hist_init_agg_state(fcinfo);

	runs = LOG2_HIST_RUNS(h);
	counts = (const void *) LOG2_HIST_COUNTS(h);

	for (int r = 0; r < h->num_runs; r++)
	{
		uint8		skip = runs[r].skip_zeros;
		uint8		len = runs[r].run_len;

		b_idx += skip;
		if (unlikely(len == 0 ||
					 b_idx + len > LOG2_HIST_NUM_BUCKETS ||
					 v_idx + len > h->num_values))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted log2_hist in log2_hist_merge_agg")));

		for (int k = 0; k < len; k++)
		{
			state->buckets[b_idx] = sat_add_u64(state->buckets[b_idx],
												log2_hist_read_u64(counts, v_idx++));
			b_idx++;
		}
	}

	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(log2_hist_agg_combinefn);
Datum
log2_hist_agg_combinefn(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state1;
	Log2HistAggState *state2;

	state1 = PG_ARGISNULL(0) ? NULL : (Log2HistAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (Log2HistAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
	{
		if (state1 == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state1);
	}

	if (state1 == NULL)
	{
		state1 = log2_hist_init_agg_state(fcinfo);
		memcpy(state1->buckets, state2->buckets, sizeof(state1->buckets));
		PG_RETURN_POINTER(state1);
	}

	log2_hist_add_buckets_sat(state1->buckets, state2->buckets);
	PG_RETURN_POINTER(state1);
}

PG_FUNCTION_INFO_V1(log2_hist_agg_serialfn);
Datum
log2_hist_agg_serialfn(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state;
	Log2Hist   *h;
	const Log2HistRun *runs;
	const void *counts;
	StringInfoData buf;

	if (!AggCheckCallContext(fcinfo, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("log2_hist aggregate serialization function called in non-aggregate context")));

	state = (Log2HistAggState *) PG_GETARG_POINTER(0);
	h = log2_hist_compress(state->buckets);
	runs = LOG2_HIST_RUNS(h);
	counts = (const void *) LOG2_HIST_COUNTS(h);

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, h->num_runs);
	pq_sendbyte(&buf, h->num_values);
	pq_sendint16(&buf, h->flags);

	for (int r = 0; r < h->num_runs; r++)
	{
		pq_sendbyte(&buf, runs[r].skip_zeros);
		pq_sendbyte(&buf, runs[r].run_len);
	}

	for (int v = 0; v < h->num_values; v++)
		pq_sendint64(&buf, (int64) log2_hist_read_u64(counts, v));

	pfree(h);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(log2_hist_agg_deserialfn);
Datum
log2_hist_agg_deserialfn(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	Log2HistAggState *state;
	StringInfoData buf;
	uint8		num_runs;
	uint8		num_values;
	uint16		flags;
	Log2HistRun runs[LOG2_HIST_MAX_RUNS];
	int			bucket_idx = 0;
	int			val_idx = 0;

	if (!AggCheckCallContext(fcinfo, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("log2_hist aggregate deserialization function called in non-aggregate context")));

	sstate = PG_GETARG_BYTEA_PP(0);
	buf.data = VARDATA_ANY(sstate);
	buf.len = VARSIZE_ANY_EXHDR(sstate);
	buf.maxlen = buf.len;
	buf.cursor = 0;

	num_runs = pq_getmsgbyte(&buf);
	num_values = pq_getmsgbyte(&buf);
	flags = pq_getmsgint(&buf, sizeof(uint16));

	if (num_runs > LOG2_HIST_MAX_RUNS ||
		num_values > LOG2_HIST_NUM_BUCKETS ||
		flags != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid binary header in log2_hist aggregate deserialization")));

	for (int r = 0; r < num_runs; r++)
	{
		runs[r].skip_zeros = pq_getmsgbyte(&buf);
		runs[r].run_len = pq_getmsgbyte(&buf);
	}

	/*
	 * Allocate deserialized state in CurrentMemoryContext (per-input-tuple
	 * memory context set up by EEOP_AGG_DESERIALIZE), NOT aggcontext, so
	 * temporary deserialized states are reclaimed after combinefn.
	 */
	state = (Log2HistAggState *) palloc0(sizeof(Log2HistAggState));

	for (int r = 0; r < num_runs; r++)
	{
		uint8		skip = runs[r].skip_zeros;
		uint8		len = runs[r].run_len;

		bucket_idx += skip;
		if (len == 0 ||
			bucket_idx + len > LOG2_HIST_NUM_BUCKETS ||
			val_idx + len > num_values)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid RLE run in log2_hist aggregate deserialization")));

		for (int k = 0; k < len; k++)
		{
			state->buckets[bucket_idx++] = (uint64) pq_getmsgint64(&buf);
			val_idx++;
		}
	}

	if (val_idx != num_values)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("mismatched value count in log2_hist aggregate deserialization")));

	pq_getmsgend(&buf);
	PG_FREE_IF_COPY(sstate, 0);

	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(log2_hist_agg_finalfn);
Datum
log2_hist_agg_finalfn(PG_FUNCTION_ARGS)
{
	Log2HistAggState *state;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	state = (Log2HistAggState *) PG_GETARG_POINTER(0);
	PG_RETURN_LOG2_HIST_P(log2_hist_compress(state->buckets));
}
