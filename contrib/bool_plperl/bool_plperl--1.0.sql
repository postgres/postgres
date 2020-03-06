/* contrib/bool_plperl/bool_plperl--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION bool_plperl" to load this file. \quit

CREATE FUNCTION bool_to_plperl(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_to_bool(val internal) RETURNS bool
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR bool LANGUAGE plperl (
    FROM SQL WITH FUNCTION bool_to_plperl(internal),
    TO SQL WITH FUNCTION plperl_to_bool(internal)
);

COMMENT ON TRANSFORM FOR bool LANGUAGE plperl IS 'transform between bool and Perl';
