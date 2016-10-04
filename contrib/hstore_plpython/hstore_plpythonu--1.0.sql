/* contrib/hstore_plpython/hstore_plpythonu--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plpythonu" to load this file. \quit

CREATE FUNCTION hstore_to_plpython(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE plpythonu (
    FROM SQL WITH FUNCTION hstore_to_plpython(internal),
    TO SQL WITH FUNCTION plpython_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR hstore LANGUAGE plpythonu IS 'transform between hstore and Python dict';
