/* src/pl/plpython/plpython3u--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE PROCEDURAL LANGUAGE plpython3u;

COMMENT ON PROCEDURAL LANGUAGE plpython3u IS 'PL/Python3U untrusted procedural language';
