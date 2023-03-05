/* src/test/modules/test_extensions/test_ext_cor--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_cor" to load this file. \quit

-- It's generally bad style to use CREATE OR REPLACE unnecessarily.
-- Test what happens if an extension does it anyway.

CREATE OR REPLACE FUNCTION ext_cor_func() RETURNS text
  AS $$ SELECT 'ext_cor_func: from extension'::text $$ LANGUAGE sql;

CREATE OR REPLACE VIEW ext_cor_view AS
  SELECT 'ext_cor_view: from extension'::text AS col;

-- These are for testing replacement of a shell type/operator, which works
-- enough like an implicit OR REPLACE to be important to check.

CREATE TYPE test_ext_type AS ENUM('x', 'y');

CREATE OPERATOR <<@@ ( PROCEDURE = pt_contained_poly,
  LEFTARG = point, RIGHTARG = polygon );
