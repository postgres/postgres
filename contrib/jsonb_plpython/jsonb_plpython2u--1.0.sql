/* contrib/jsonb_plpython/jsonb_plpython2u--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plpython2u" to load this file. \quit

CREATE FUNCTION jsonb_to_plpython2(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'jsonb_to_plpython';

CREATE FUNCTION plpython2_to_jsonb(val internal) RETURNS jsonb
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plpython_to_jsonb';

CREATE TRANSFORM FOR jsonb LANGUAGE plpython2u (
    FROM SQL WITH FUNCTION jsonb_to_plpython2(internal),
    TO SQL WITH FUNCTION plpython2_to_jsonb(internal)
);

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plpython2u IS 'transform between jsonb and Python';
