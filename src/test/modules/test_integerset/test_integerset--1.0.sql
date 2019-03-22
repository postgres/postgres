/* src/test/modules/test_integerset/test_integerset--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_integerset" to load this file. \quit

CREATE FUNCTION test_integerset()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
