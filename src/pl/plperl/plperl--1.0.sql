/* src/pl/plperl/plperl--1.0.sql */

CREATE FUNCTION plperl_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_inline_handler(internal) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE FUNCTION plperl_validator(oid) RETURNS void
  STRICT LANGUAGE c AS 'MODULE_PATHNAME';

CREATE TRUSTED LANGUAGE plperl
  HANDLER plperl_call_handler
  INLINE plperl_inline_handler
  VALIDATOR plperl_validator;

-- The language object, but not the functions, can be owned by a non-superuser.
ALTER LANGUAGE plperl OWNER TO @extowner@;

COMMENT ON LANGUAGE plperl IS 'PL/Perl procedural language';
