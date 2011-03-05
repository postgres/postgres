/* src/pl/plpython/plpythonu--unpackaged--1.0.sql */

ALTER EXTENSION plpythonu ADD PROCEDURAL LANGUAGE plpythonu;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plpythonu ADD FUNCTION plpython_call_handler();
ALTER EXTENSION plpythonu ADD FUNCTION plpython_inline_handler(internal);
ALTER EXTENSION plpythonu ADD FUNCTION plpython_validator(oid);
