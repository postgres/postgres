/* src/pl/plperl/plperlu--1.0.sql */

CREATE FUNCTION plperlu_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plperlu_inline_handler(internal) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plperlu_validator(oid) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE LANGUAGE plperlu
  HANDLER plperlu_call_handler
  INLINE plperlu_inline_handler
  VALIDATOR plperlu_validator;

COMMENT ON LANGUAGE plperlu IS 'PL/PerlU untrusted procedural language';
