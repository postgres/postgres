/* contrib/jsonb_plperl/jsonb_plperl--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plperl" to load this file. \quit

CREATE FUNCTION jsonb_to_plperl(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_to_jsonb(val internal) RETURNS jsonb
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR jsonb LANGUAGE plperl (
    FROM SQL WITH FUNCTION jsonb_to_plperl(internal),
    TO SQL WITH FUNCTION plperl_to_jsonb(internal)
);

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plperl IS 'transform between jsonb and Perl';
