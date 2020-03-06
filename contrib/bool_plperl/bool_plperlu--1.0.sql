/* contrib/bool_plperl/bool_plperlu--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION bool_plperlu" to load this file. \quit

CREATE FUNCTION bool_to_plperlu(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'bool_to_plperl';

CREATE FUNCTION plperlu_to_bool(val internal) RETURNS bool
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'plperl_to_bool';

CREATE TRANSFORM FOR bool LANGUAGE plperlu (
    FROM SQL WITH FUNCTION bool_to_plperlu(internal),
    TO SQL WITH FUNCTION plperlu_to_bool(internal)
);

COMMENT ON TRANSFORM FOR bool LANGUAGE plperlu IS 'transform between bool and Perl';
