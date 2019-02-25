/* src/pl/plpgsql/src/plpgsql--unpackaged--1.0.sql */

ALTER EXTENSION plpgsql ADD LANGUAGE plpgsql;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION plpgsql ADD FUNCTION plpgsql_call_handler();
ALTER EXTENSION plpgsql ADD FUNCTION plpgsql_inline_handler(internal);
ALTER EXTENSION plpgsql ADD FUNCTION plpgsql_validator(oid);
