/* src/pl/tcl/pltclu--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE PROCEDURAL LANGUAGE pltclu;

COMMENT ON PROCEDURAL LANGUAGE pltclu IS 'PL/TclU untrusted procedural language';
