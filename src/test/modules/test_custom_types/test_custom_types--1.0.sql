/* src/test/modules/test_custom_types/test_custom_types--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_custom_types" to load this file. \quit

--
-- Input/output functions for int_custom type
--
CREATE FUNCTION int_custom_in(cstring)
RETURNS int_custom
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_out(int_custom)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

--
-- Typanalyze function that returns false
--
CREATE FUNCTION int_custom_typanalyze_false(internal)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- Typanalyze function that returns invalid stats
--
CREATE FUNCTION int_custom_typanalyze_invalid(internal)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- The int_custom type definition
--
-- This type is identical to int4 in storage, and is used in subsequent
-- tests to have different properties.
--
CREATE TYPE int_custom (
  INPUT = int_custom_in,
  OUTPUT = int_custom_out,
  LIKE = int4
);

--
-- Comparison functions for int_custom
--
-- These are required to create a btree operator class, which is needed
-- for the type to be usable in extended statistics objects.
--
CREATE FUNCTION int_custom_eq(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_ne(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_lt(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_le(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_gt(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_ge(int_custom, int_custom)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION int_custom_cmp(int_custom, int_custom)
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- Operators for int_custom, for btree operator class
CREATE OPERATOR = (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_eq,
  COMMUTATOR = =,
  NEGATOR = <>,
  RESTRICT = eqsel,
  JOIN = eqjoinsel,
  HASHES,
  MERGES
);

CREATE OPERATOR <> (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_ne,
  COMMUTATOR = <>,
  NEGATOR = =,
  RESTRICT = neqsel,
  JOIN = neqjoinsel
);

CREATE OPERATOR < (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_lt,
  COMMUTATOR = >,
  NEGATOR = >=,
  RESTRICT = scalarltsel,
  JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_le,
  COMMUTATOR = >=,
  NEGATOR = >,
  RESTRICT = scalarlesel,
  JOIN = scalarlejoinsel
);

CREATE OPERATOR > (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_gt,
  COMMUTATOR = <,
  NEGATOR = <=,
  RESTRICT = scalargtsel,
  JOIN = scalargtjoinsel
);

CREATE OPERATOR >= (
  LEFTARG = int_custom,
  RIGHTARG = int_custom,
  FUNCTION = int_custom_ge,
  COMMUTATOR = <=,
  NEGATOR = <,
  RESTRICT = scalargesel,
  JOIN = scalargejoinsel
);

--
-- Btree operator class for int_custom
--
-- This is required for the type to be usable in extended statistics objects,
-- for attributes and expressions.
--
CREATE OPERATOR CLASS int_custom_ops
  DEFAULT FOR TYPE int_custom USING btree AS
    OPERATOR    1     <,
    OPERATOR    2     <=,
    OPERATOR    3     =,
    OPERATOR    4     >=,
    OPERATOR    5     >,
    FUNCTION    1     int_custom_cmp(int_custom, int_custom);
