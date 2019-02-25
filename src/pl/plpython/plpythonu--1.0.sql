/* src/pl/plpython/plpythonu--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE LANGUAGE plpythonu;

COMMENT ON LANGUAGE plpythonu IS 'PL/PythonU untrusted procedural language';
