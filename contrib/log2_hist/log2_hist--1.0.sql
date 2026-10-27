/* contrib/log2_hist/log2_hist--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION log2_hist" to load this file. \quit

-- Shell type declaration
CREATE TYPE log2_hist;

-- I/O and binary wire functions
CREATE FUNCTION log2_hist_in(cstring)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_in'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_out(log2_hist)
RETURNS cstring
AS 'MODULE_PATHNAME', 'log2_hist_out'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_recv(internal)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_recv'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_send(log2_hist)
RETURNS bytea
AS 'MODULE_PATHNAME', 'log2_hist_send'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Subscripting handler (0..63 raw indexing and slicing)
CREATE FUNCTION log2_hist_subscript_handler(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_subscript_handler'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Base type definition
CREATE TYPE log2_hist (
    INPUT = log2_hist_in,
    OUTPUT = log2_hist_out,
    RECEIVE = log2_hist_recv,
    SEND = log2_hist_send,
    SUBSCRIPT = log2_hist_subscript_handler,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT = double,
    STORAGE = main
);

CREATE FUNCTION log2_hist_from_bytea(bytea)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_from_bytea'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Cast functions between log2_hist and bigint[]
CREATE FUNCTION log2_hist_to_int8array(log2_hist)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'log2_hist_to_int8array'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION int8array_to_log2_hist(bigint[])
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'int8array_to_log2_hist'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE CAST (log2_hist AS bigint[])
    WITH FUNCTION log2_hist_to_int8array(log2_hist)
    AS ASSIGNMENT;

CREATE CAST (bigint[] AS log2_hist)
    WITH FUNCTION int8array_to_log2_hist(bigint[])
    AS ASSIGNMENT;

-- Full 64-element expansion & configurable array conversion
CREATE FUNCTION log2_hist_buckets(h log2_hist)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'log2_hist_buckets'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_to_array(
    h log2_hist,
    full_range boolean DEFAULT true,
    index_base integer DEFAULT 0
)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'log2_hist_to_array'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Bucket index computation: 64 - clz64(val) -> 0..63
CREATE FUNCTION log2_hist_bucket(val bigint)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_bucket_1'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_bucket(val bigint, clamp_negative boolean)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_bucket_2'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Constructors and single-observation builders
CREATE FUNCTION log2_hist_empty()
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_empty'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_single(val bigint)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_single_1'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_single(
    val bigint,
    clamp_negative boolean,
    count bigint DEFAULT 1
)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_single_3'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Observation addition & histogram merging
CREATE FUNCTION log2_hist_add(h log2_hist, val bigint)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_add_2'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_add_rev(val bigint, h log2_hist)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_add_rev'
LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_add(
    h log2_hist,
    val bigint,
    clamp_negative boolean,
    count bigint DEFAULT 1
)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_add_4'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_merge(a log2_hist, b log2_hist)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_merge'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_add(a log2_hist, b log2_hist)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_merge'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Inspection & analytics functions
CREATE FUNCTION log2_hist_total_count(h log2_hist)
RETURNS bigint
AS 'MODULE_PATHNAME', 'log2_hist_total_count'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_min_bucket(h log2_hist)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_min_bucket'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_max_bucket(h log2_hist)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_max_bucket'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_percentile(h log2_hist, p double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'log2_hist_percentile'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Comparison & Hashing functions
CREATE FUNCTION log2_hist_eq(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_eq'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_ne(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_ne'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_lt(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_lt'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_le(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_le'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_ge(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_ge'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_gt(a log2_hist, b log2_hist)
RETURNS boolean
AS 'MODULE_PATHNAME', 'log2_hist_gt'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_cmp(a log2_hist, b log2_hist)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_cmp'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_hash(h log2_hist)
RETURNS integer
AS 'MODULE_PATHNAME', 'log2_hist_hash'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_hash_extended(h log2_hist, seed bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'log2_hist_hash_extended'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Operators
CREATE OPERATOR + (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_merge,
    COMMUTATOR = +
);

CREATE OPERATOR + (
    LEFTARG = log2_hist,
    RIGHTARG = bigint,
    FUNCTION = log2_hist_add,
    COMMUTATOR = +
);

CREATE OPERATOR + (
    LEFTARG = bigint,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_add_rev,
    COMMUTATOR = +
);

CREATE OPERATOR = (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_eq,
    COMMUTATOR = =,
    NEGATOR = <>,
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    HASHES,
    MERGES
);

CREATE OPERATOR <> (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_ne,
    COMMUTATOR = <>,
    NEGATOR = =,
    RESTRICT = neqsel,
    JOIN = neqjoinsel
);

CREATE OPERATOR < (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_lt,
    COMMUTATOR = >,
    NEGATOR = >=,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_le,
    COMMUTATOR = >=,
    NEGATOR = >,
    RESTRICT = scalarlesel,
    JOIN = scalarlejoinsel
);

CREATE OPERATOR >= (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_ge,
    COMMUTATOR = <=,
    NEGATOR = <,
    RESTRICT = scalargesel,
    JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = log2_hist,
    RIGHTARG = log2_hist,
    FUNCTION = log2_hist_gt,
    COMMUTATOR = <,
    NEGATOR = <=,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel
);

-- Default B-tree and Hash operator classes
CREATE OPERATOR CLASS log2_hist_btree_ops
    DEFAULT FOR TYPE log2_hist USING btree AS
        OPERATOR 1 < ,
        OPERATOR 2 <= ,
        OPERATOR 3 = ,
        OPERATOR 4 >= ,
        OPERATOR 5 > ,
        FUNCTION 1 log2_hist_cmp(log2_hist, log2_hist);

CREATE OPERATOR CLASS log2_hist_hash_ops
    DEFAULT FOR TYPE log2_hist USING hash AS
        OPERATOR 1 = ,
        FUNCTION 1 log2_hist_hash(log2_hist),
        FUNCTION 2 log2_hist_hash_extended(log2_hist, bigint);

-- Parallel-safe aggregate support functions
CREATE FUNCTION log2_hist_agg_transfn(internal, bigint)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_agg_transfn_1'
LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_agg_transfn(internal, bigint, boolean)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_agg_transfn_2'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_merge_agg_transfn(internal, log2_hist)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_merge_agg_transfn'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_agg_combinefn(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_agg_combinefn'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_agg_serialfn(internal)
RETURNS bytea
AS 'MODULE_PATHNAME', 'log2_hist_agg_serialfn'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_agg_deserialfn(bytea, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'log2_hist_agg_deserialfn'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION log2_hist_agg_finalfn(internal)
RETURNS log2_hist
AS 'MODULE_PATHNAME', 'log2_hist_agg_finalfn'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Parallel-safe Aggregates
CREATE AGGREGATE log2_hist_agg(val bigint) (
    SFUNC = log2_hist_agg_transfn,
    STYPE = internal,
    SSPACE = 512,
    FINALFUNC = log2_hist_agg_finalfn,
    COMBINEFUNC = log2_hist_agg_combinefn,
    SERIALFUNC = log2_hist_agg_serialfn,
    DESERIALFUNC = log2_hist_agg_deserialfn,
    PARALLEL = SAFE
);

CREATE AGGREGATE log2_hist_agg(val bigint, clamp_negative boolean) (
    SFUNC = log2_hist_agg_transfn,
    STYPE = internal,
    SSPACE = 512,
    FINALFUNC = log2_hist_agg_finalfn,
    COMBINEFUNC = log2_hist_agg_combinefn,
    SERIALFUNC = log2_hist_agg_serialfn,
    DESERIALFUNC = log2_hist_agg_deserialfn,
    PARALLEL = SAFE
);

CREATE AGGREGATE log2_hist_merge_agg(h log2_hist) (
    SFUNC = log2_hist_merge_agg_transfn,
    STYPE = internal,
    SSPACE = 512,
    FINALFUNC = log2_hist_agg_finalfn,
    COMBINEFUNC = log2_hist_agg_combinefn,
    SERIALFUNC = log2_hist_agg_serialfn,
    DESERIALFUNC = log2_hist_agg_deserialfn,
    PARALLEL = SAFE
);
