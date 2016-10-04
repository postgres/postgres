/* contrib/hstore_plperl/hstore_plperl--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plperl" to load this file. \quit

CREATE FUNCTION hstore_to_plperl(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE plperl (
    FROM SQL WITH FUNCTION hstore_to_plperl(internal),
    TO SQL WITH FUNCTION plperl_to_hstore(internal)
);
