/* src/pl/tcl/pltclu--unpackaged--1.0.sql */

ALTER EXTENSION pltclu ADD LANGUAGE pltclu;
-- ALTER ADD LANGUAGE doesn't pick up the support functions, so we have to.
ALTER EXTENSION pltclu ADD FUNCTION pltclu_call_handler();
