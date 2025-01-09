/* contrib/hstore_plperl/hstore_plperlu--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plperlu" to load this file. \quit

CREATE FUNCTION hstore_to_plperlu(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'hstore_to_plperl';

CREATE FUNCTION plperlu_to_hstore(val internal) RETURNS @extschema:hstore@.hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plperl_to_hstore';

CREATE TRANSFORM FOR @extschema:hstore@.hstore LANGUAGE plperlu (
    FROM SQL WITH FUNCTION hstore_to_plperlu(internal),
    TO SQL WITH FUNCTION plperlu_to_hstore(internal)
);
