/* src/pl/plpython/plpython3u--unpackaged--1.0.sql */

ALTER EXTENSION plpython3u ADD LANGUAGE plpython3u;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plpython3u ADD FUNCTION plpython3_call_handler();
ALTER EXTENSION plpython3u ADD FUNCTION plpython3_inline_handler(internal);
ALTER EXTENSION plpython3u ADD FUNCTION plpython3_validator(oid);
