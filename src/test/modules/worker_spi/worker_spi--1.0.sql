/* src/test/modules/worker_spi/worker_spi--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION worker_spi" to load this file. \quit

-- In the default case, dboid and roleoid fall back to their respective GUCs.
CREATE FUNCTION worker_spi_launch(index int4,
  dboid oid DEFAULT 0,
  roleoid oid DEFAULT 0,
  flags text[] DEFAULT '{}')
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;
