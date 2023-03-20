/* src/test/modules/test_extensions/test_ext_req_schema2--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_req_schema2" to load this file. \quit

-- This formulation can handle relocation of the required extension.
CREATE FUNCTION dep_req2() RETURNS text
BEGIN ATOMIC
  SELECT @extschema:test_ext_req_schema1@.dep_req1() || ' req2';
END;
