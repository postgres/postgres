/* contrib/jsonb_plperl/jsonb_plperlu--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plperlu" to load this file. \quit

CREATE FUNCTION jsonb_to_plperlu(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'jsonb_to_plperl';

CREATE FUNCTION plperlu_to_jsonb(val internal) RETURNS jsonb
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plperl_to_jsonb';

CREATE TRANSFORM FOR jsonb LANGUAGE plperlu (
    FROM SQL WITH FUNCTION jsonb_to_plperlu(internal),
    TO SQL WITH FUNCTION plperlu_to_jsonb(internal)
);

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plperlu IS 'transform between jsonb and Perl';
