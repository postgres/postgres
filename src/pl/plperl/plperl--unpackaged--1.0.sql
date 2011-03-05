/* src/pl/plperl/plperl--unpackaged--1.0.sql */

ALTER EXTENSION plperl ADD PROCEDURAL LANGUAGE plperl;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plperl ADD FUNCTION plperl_call_handler();
ALTER EXTENSION plperl ADD FUNCTION plperl_inline_handler(internal);
ALTER EXTENSION plperl ADD FUNCTION plperl_validator(oid);
