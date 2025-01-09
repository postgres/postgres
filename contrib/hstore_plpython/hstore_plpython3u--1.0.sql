/* contrib/hstore_plpython/hstore_plpython3u--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plpython3u" to load this file. \quit

CREATE FUNCTION hstore_to_plpython3(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'hstore_to_plpython';

CREATE FUNCTION plpython3_to_hstore(val internal) RETURNS @extschema:hstore@.hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plpython_to_hstore';

CREATE TRANSFORM FOR @extschema:hstore@.hstore LANGUAGE plpython3u (
    FROM SQL WITH FUNCTION hstore_to_plpython3(internal),
    TO SQL WITH FUNCTION plpython3_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR @extschema:hstore@.hstore LANGUAGE plpython3u IS 'transform between hstore and Python dict';
