/* contrib/worker_spi/worker_spi--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION worker_spi" to load this file. \quit

CREATE FUNCTION worker_spi_launch(pg_catalog.int4)
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;
