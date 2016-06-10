/* contrib/pg_stat_statements/pg_stat_statements--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.4'" to load this file. \quit

ALTER FUNCTION pg_stat_statements_reset() PARALLEL SAFE;
ALTER FUNCTION pg_stat_statements(boolean) PARALLEL SAFE;
