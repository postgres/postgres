/* contrib/jsonb_plpython/jsonb_plpythonu--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plpythonu" to load this file. \quit

CREATE FUNCTION jsonb_to_plpython(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython_to_jsonb(val internal) RETURNS jsonb
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR jsonb LANGUAGE plpythonu (
    FROM SQL WITH FUNCTION jsonb_to_plpython(internal),
    TO SQL WITH FUNCTION plpython_to_jsonb(internal)
);

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plpythonu IS 'transform between jsonb and Python';
