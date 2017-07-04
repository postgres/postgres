/* contrib/hstore_plpython/hstore_plpython2u--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plpython2u" to load this file. \quit

CREATE FUNCTION hstore_to_plpython2(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'hstore_to_plpython';

CREATE FUNCTION plpython2_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plpython_to_hstore';

CREATE TRANSFORM FOR hstore LANGUAGE plpython2u (
    FROM SQL WITH FUNCTION hstore_to_plpython2(internal),
    TO SQL WITH FUNCTION plpython2_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR hstore LANGUAGE plpython2u IS 'transform between hstore and Python dict';
