/* src/pl/plperl/plperlu--unpackaged--1.0.sql */

ALTER EXTENSION plperlu ADD LANGUAGE plperlu;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plperlu ADD FUNCTION plperlu_call_handler();
ALTER EXTENSION plperlu ADD FUNCTION plperlu_inline_handler(internal);
ALTER EXTENSION plperlu ADD FUNCTION plperlu_validator(oid);
