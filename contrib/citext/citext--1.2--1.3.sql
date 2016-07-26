/* contrib/citext/citext--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.3'" to load this file. \quit

UPDATE pg_aggregate SET aggcombinefn = 'citext_smaller'
WHERE aggfnoid = 'min(citext)'::pg_catalog.regprocedure;
