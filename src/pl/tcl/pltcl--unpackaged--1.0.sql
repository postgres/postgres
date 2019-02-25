/* src/pl/tcl/pltcl--unpackaged--1.0.sql */

ALTER EXTENSION pltcl ADD LANGUAGE pltcl;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION pltcl ADD FUNCTION pltcl_call_handler();
