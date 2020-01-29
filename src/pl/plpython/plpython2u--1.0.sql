/* src/pl/plpython/plpython2u--1.0.sql */

CREATE FUNCTION plpython2_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython2_inline_handler(internal) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython2_validator(oid) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE LANGUAGE plpython2u
  HANDLER plpython2_call_handler
  INLINE plpython2_inline_handler
  VALIDATOR plpython2_validator;

COMMENT ON LANGUAGE plpython2u IS 'PL/Python2U untrusted procedural language';
