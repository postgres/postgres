/* src/test/modules/test_extensions/test_ext_req_schema1--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_req_schema1" to load this file. \quit

CREATE FUNCTION dep_req1() RETURNS text
LANGUAGE SQL AS $$ SELECT 'req1' $$;
