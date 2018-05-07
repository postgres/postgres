/* contrib/adminpack/adminpack--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION adminpack UPDATE TO '1.1'" to load this file. \quit

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_logfile_rotate() FROM PUBLIC;
