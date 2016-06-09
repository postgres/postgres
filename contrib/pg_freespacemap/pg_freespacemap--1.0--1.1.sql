/* contrib/pg_freespacemap/pg_freespacemap--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_freespacemap UPDATE TO '1.1'" to load this file. \quit

ALTER FUNCTION pg_freespace(regclass, bigint) PARALLEL SAFE;
ALTER FUNCTION pg_freespace(regclass) PARALLEL SAFE;
