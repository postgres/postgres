/* contrib/pg_trgm/pg_trgm--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.4'" to load this file. \quit

CREATE FUNCTION strict_word_similarity(text,text)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION strict_word_similarity_op(text,text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE PARALLEL SAFE;  -- stable because depends on pg_trgm.word_similarity_threshold

CREATE FUNCTION strict_word_similarity_commutator_op(text,text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE PARALLEL SAFE;  -- stable because depends on pg_trgm.word_similarity_threshold

CREATE OPERATOR <<% (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = strict_word_similarity_op,
        COMMUTATOR = '%>>',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE OPERATOR %>> (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = strict_word_similarity_commutator_op,
        COMMUTATOR = '<<%',
        RESTRICT = contsel,
        JOIN = contjoinsel
);

CREATE FUNCTION strict_word_similarity_dist_op(text,text)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION strict_word_similarity_dist_commutator_op(text,text)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <<<-> (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = strict_word_similarity_dist_op,
        COMMUTATOR = '<->>>'
);

CREATE OPERATOR <->>> (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = strict_word_similarity_dist_commutator_op,
        COMMUTATOR = '<<<->'
);

ALTER OPERATOR FAMILY gist_trgm_ops USING gist ADD
        OPERATOR        9       %>> (text, text),
        OPERATOR        10       <->>> (text, text) FOR ORDER BY pg_catalog.float_ops;

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        OPERATOR        9       %>> (text, text);
