/* src/pl/plperl/plperl--1.0.sql */

/*
 * Currently, all the interesting stuff is done by CREATE LANGUAGE.
 * Later we will probably "dumb down" that command and put more of the
 * knowledge into this script.
 */

CREATE PROCEDURAL LANGUAGE plperl;

COMMENT ON PROCEDURAL LANGUAGE plperl IS 'PL/Perl procedural language';
