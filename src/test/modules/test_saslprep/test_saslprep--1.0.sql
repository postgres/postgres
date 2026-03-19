/* src/test/modules/test_saslprep/test_saslprep--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_saslprep" to load this file. \quit

--
-- test_saslprep(bytea)
--
-- Tests single byte sequence in SASLprep.
--
CREATE FUNCTION test_saslprep(IN src bytea,
    OUT res bytea,
    OUT status text)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- test_saslprep_ranges
--
-- Tests all possible ranges of byte sequences in SASLprep.
--
CREATE FUNCTION test_saslprep_ranges(
    OUT codepoint text,
    OUT status text,
    OUT src bytea,
    OUT res bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
