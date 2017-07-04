/* contrib/pg_freespacemap/pg_freespacemap--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_freespacemap UPDATE TO '1.2'" to load this file. \quit

GRANT EXECUTE ON FUNCTION  pg_freespace(regclass, bigint) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION  pg_freespace(regclass) TO pg_stat_scan_tables;
