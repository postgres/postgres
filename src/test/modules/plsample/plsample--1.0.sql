/* src/test/modules/plsample/plsample--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plsample" to load this file. \quit

CREATE FUNCTION plsample_call_handler() RETURNS language_handler
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE TRUSTED LANGUAGE plsample
  HANDLER plsample_call_handler;

ALTER LANGUAGE plsample OWNER TO @extowner@;

COMMENT ON LANGUAGE plsample IS 'PL/Sample procedural language';
