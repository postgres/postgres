/* src/test/modules/test_extensions/test_ext_req_schema3--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_req_schema3" to load this file. \quit

-- This formulation cannot handle relocation of the required extension.
CREATE FUNCTION dep_req3() RETURNS text
LANGUAGE SQL IMMUTABLE PARALLEL SAFE
AS $$ SELECT @extschema:test_ext_req_schema1@.dep_req1() || ' req3' $$;

CREATE FUNCTION dep_req3b() RETURNS text
LANGUAGE SQL IMMUTABLE PARALLEL SAFE
AS $$ SELECT @extschema:test_ext_req_schema2@.dep_req2() || ' req3b' $$;
