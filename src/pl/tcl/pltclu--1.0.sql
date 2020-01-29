/* src/pl/tcl/pltclu--1.0.sql */

CREATE FUNCTION pltclu_call_handler() RETURNS language_handler
  LANGUAGE c AS 'MODULE_PATHNAME';

CREATE LANGUAGE pltclu
  HANDLER pltclu_call_handler;

COMMENT ON LANGUAGE pltclu IS 'PL/TclU untrusted procedural language';
