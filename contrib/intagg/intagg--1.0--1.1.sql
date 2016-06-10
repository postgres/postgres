/* contrib/intagg/intagg--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intagg UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION int_agg_state(internal, int4) PARALLEL SAFE;
ALTER FUNCTION int_agg_final_array(internal) PARALLEL SAFE;
ALTER FUNCTION int_array_enum(int4[]) PARALLEL SAFE;

UPDATE pg_proc SET proparallel = 's'
WHERE oid = 'int_array_aggregate(int4)'::pg_catalog.regprocedure;
