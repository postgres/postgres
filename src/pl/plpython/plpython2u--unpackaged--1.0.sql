/* src/pl/plpython/plpython2u--unpackaged--1.0.sql */

ALTER EXTENSION plpython2u ADD LANGUAGE plpython2u;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plpython2u ADD FUNCTION plpython2_call_handler();
ALTER EXTENSION plpython2u ADD FUNCTION plpython2_inline_handler(internal);
ALTER EXTENSION plpython2u ADD FUNCTION plpython2_validator(oid);
