/* contrib/pg_stat_statements/pg_stat_statements--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_statements FROM unpackaged" to load this file. \quit

ALTER EXTENSION pg_stat_statements ADD function pg_stat_statements_reset();
ALTER EXTENSION pg_stat_statements ADD function pg_stat_statements();
ALTER EXTENSION pg_stat_statements ADD view pg_stat_statements;
