/* src/test/modules/test_wait_lsn/test_wait_lsn--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_wait_lsn" to load this file. \quit

CREATE FUNCTION test_wait_lsn_wakeup(
    pg_catalog.text, pg_catalog.pg_lsn)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_wait_lsn_waiter_is_registered(
    pg_catalog.int4, pg_catalog.text, pg_catalog.pg_lsn)
RETURNS pg_catalog.bool STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
