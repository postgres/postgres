/* contrib/pg_stat_statements/pg_stat_statements--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.5'" to load this file. \quit

GRANT EXECUTE ON FUNCTION pg_stat_statements_reset() TO pg_read_all_stats;
