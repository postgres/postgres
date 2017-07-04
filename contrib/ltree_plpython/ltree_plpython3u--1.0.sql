/* contrib/ltree_plpython/ltree_plpython3u--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION ltree_plpython3u" to load this file. \quit

CREATE FUNCTION ltree_to_plpython3(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME', 'ltree_to_plpython';

CREATE TRANSFORM FOR ltree LANGUAGE plpython3u (
    FROM SQL WITH FUNCTION ltree_to_plpython3(internal)
);
