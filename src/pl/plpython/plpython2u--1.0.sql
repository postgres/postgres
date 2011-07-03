/* src/pl/plpython/plpython2u--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE PROCEDURAL LANGUAGE plpython2u;

COMMENT ON PROCEDURAL LANGUAGE plpython2u IS 'PL/Python2U untrusted procedural language';
