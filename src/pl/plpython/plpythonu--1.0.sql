/* src/pl/plpython/plpythonu--1.0.sql */

CREATE FUNCTION plpython_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython_inline_handler(internal) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython_validator(oid) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE LANGUAGE plpythonu
  HANDLER plpython_call_handler
  INLINE plpython_inline_handler
  VALIDATOR plpython_validator;

COMMENT ON LANGUAGE plpythonu IS 'PL/PythonU untrusted procedural language';
