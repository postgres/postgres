/* src/test/modules/test_lwlock_tranches/test_lwlock_tranches--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_lwlock_tranches" to load this file. \quit

CREATE FUNCTION test_lwlock_tranches() RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_lwlock_tranche_creation(tranche_name TEXT) RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_lwlock_tranche_lookup(tranche_name TEXT) RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_lwlock_initialize(tranche_id INT) RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;
