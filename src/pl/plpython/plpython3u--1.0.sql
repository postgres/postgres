/* src/pl/plpython/plpython3u--1.0.sql */

CREATE FUNCTION plpython3_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython3_inline_handler(internal) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plpython3_validator(oid) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE LANGUAGE plpython3u
  HANDLER plpython3_call_handler
  INLINE plpython3_inline_handler
  VALIDATOR plpython3_validator;

COMMENT ON LANGUAGE plpython3u IS 'PL/Python3U untrusted procedural language';
