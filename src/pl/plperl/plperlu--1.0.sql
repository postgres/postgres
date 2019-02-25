/* src/pl/plperl/plperlu--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE LANGUAGE plperlu;

COMMENT ON LANGUAGE plperlu IS 'PL/PerlU untrusted procedural language';
