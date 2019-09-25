/* src/test/modules/dummy_index_am/dummy_index_am--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dummy_index_am" to load this file. \quit

CREATE FUNCTION dihandler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD dummy_index_am TYPE INDEX HANDLER dihandler;
COMMENT ON ACCESS METHOD dummy_index_am IS 'dummy index access method';

-- Operator classes
CREATE OPERATOR CLASS int4_ops
DEFAULT FOR TYPE int4 USING dummy_index_am AS
  OPERATOR 1 = (int4, int4),
  FUNCTION 1 hashint4(int4);
