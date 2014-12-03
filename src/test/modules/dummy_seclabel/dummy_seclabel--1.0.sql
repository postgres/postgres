/* src/test/modules/dummy_seclabel/dummy_seclabel--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dummy_seclabel" to load this file. \quit

CREATE FUNCTION dummy_seclabel_dummy()
   RETURNS pg_catalog.void
       AS 'MODULE_PATHNAME' LANGUAGE C;
