/* src/pl/tcl/pltcl--1.0.sql */

CREATE FUNCTION pltcl_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE TRUSTED LANGUAGE pltcl
  HANDLER pltcl_call_handler;

-- The language object, but not the functions, can be owned by a non-superuser.
ALTER LANGUAGE pltcl OWNER TO @extowner@;

COMMENT ON LANGUAGE pltcl IS 'PL/Tcl procedural language';
