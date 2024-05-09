/* src/test/modules/test_extensions/test_ext_set_schema--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_set_schema" to load this file. \quit

-- Create various object types that need extra handling by SET SCHEMA.

CREATE TABLE ess_table (f1 int primary key, f2 int, f3 text,
                        constraint ess_c check (f1 != f2));

CREATE FUNCTION ess_func(int) RETURNS text
BEGIN ATOMIC
  SELECT f3 FROM ess_table WHERE f1 = $1;
END;

CREATE TYPE ess_range_type AS RANGE (subtype = text);

CREATE TYPE ess_composite_type AS (f1 int, f2 ess_range_type);
