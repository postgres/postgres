/* contrib/chkpass/chkpass--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chkpass" to load this file. \quit

--
--	Input and output functions and the type itself:
--

CREATE FUNCTION chkpass_in(cstring)
	RETURNS chkpass
	AS 'MODULE_PATHNAME'
	LANGUAGE C STRICT;

CREATE FUNCTION chkpass_out(chkpass)
	RETURNS cstring
	AS 'MODULE_PATHNAME'
	LANGUAGE C STRICT;

CREATE TYPE chkpass (
	internallength = 16,
	input = chkpass_in,
	output = chkpass_out
);

CREATE FUNCTION raw(chkpass)
	RETURNS text
	AS 'MODULE_PATHNAME', 'chkpass_rout'
	LANGUAGE C STRICT;

--
--	The various boolean tests:
--

CREATE FUNCTION eq(chkpass, text)
	RETURNS bool
	AS 'MODULE_PATHNAME', 'chkpass_eq'
	LANGUAGE C STRICT;

CREATE FUNCTION ne(chkpass, text)
	RETURNS bool
	AS 'MODULE_PATHNAME', 'chkpass_ne'
	LANGUAGE C STRICT;

--
--	Now the operators.
--

CREATE OPERATOR = (
	leftarg = chkpass,
	rightarg = text,
	negator = <>,
	procedure = eq
);

CREATE OPERATOR <> (
	leftarg = chkpass,
	rightarg = text,
	negator = =,
	procedure = ne
);

COMMENT ON TYPE chkpass IS 'password type with checks';

--
--	eof
--
