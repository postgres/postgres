/* src/test/modules/test_resowner/test_resowner--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_resowner" to load this file. \quit

CREATE FUNCTION test_resowner_priorities(nkinds pg_catalog.int4, nresources pg_catalog.int4)
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_resowner_leak()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_resowner_remember_between_phases()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_resowner_forget_between_phases()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_resowner_many(
   nkinds       pg_catalog.int4,
   nremember_bl pg_catalog.int4,
   nforget_bl   pg_catalog.int4,
   nremember_al pg_catalog.int4,
   nforget_al   pg_catalog.int4
)
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;
