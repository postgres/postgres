/* contrib/pg_stat_statements/pg_stat_statements--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.6'" to load this file. \quit

-- Execution is only allowed for superusers, fixing issue with 1.5.
REVOKE EXECUTE ON FUNCTION pg_stat_statements_reset() FROM pg_read_all_stats;
