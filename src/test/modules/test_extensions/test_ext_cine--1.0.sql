/* src/test/modules/test_extensions/test_ext_cine--1.0.sql */
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_cine" to load this file. \quit

--
-- CREATE IF NOT EXISTS is an entirely unsound thing for an extension
-- to be doing, but let's at least plug the major security hole in it.
--

CREATE COLLATION IF NOT EXISTS ext_cine_coll
  ( LC_COLLATE = "POSIX", LC_CTYPE = "POSIX" );

CREATE MATERIALIZED VIEW IF NOT EXISTS ext_cine_mv AS SELECT 42 AS f1;

CREATE FOREIGN DATA WRAPPER ext_cine_fdw;

CREATE SERVER IF NOT EXISTS ext_cine_srv FOREIGN DATA WRAPPER ext_cine_fdw;

CREATE SCHEMA IF NOT EXISTS ext_cine_schema;

CREATE SEQUENCE IF NOT EXISTS ext_cine_seq;

CREATE TABLE IF NOT EXISTS ext_cine_tab1 (x int);

CREATE TABLE IF NOT EXISTS ext_cine_tab2 AS SELECT 42 AS y;
