/* src/test/modules/test_extensions/test_ext_cine--1.0--1.1.sql */
-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION test_ext_cine UPDATE TO '1.1'" to load this file. \quit

--
-- These are the same commands as in the 1.0 script; we expect them
-- to do nothing.
--

CREATE COLLATION IF NOT EXISTS ext_cine_coll
  ( LC_COLLATE = "POSIX", LC_CTYPE = "POSIX" );

CREATE MATERIALIZED VIEW IF NOT EXISTS ext_cine_mv AS SELECT 42 AS f1;

CREATE SERVER IF NOT EXISTS ext_cine_srv FOREIGN DATA WRAPPER ext_cine_fdw;

CREATE SCHEMA IF NOT EXISTS ext_cine_schema;

CREATE SEQUENCE IF NOT EXISTS ext_cine_seq;

CREATE TABLE IF NOT EXISTS ext_cine_tab1 (x int);

CREATE TABLE IF NOT EXISTS ext_cine_tab2 AS SELECT 42 AS y;

-- just to verify the script ran
CREATE TABLE ext_cine_tab3 (z int);
