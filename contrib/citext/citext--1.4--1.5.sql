/* contrib/citext/citext--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.5'" to load this file. \quit

ALTER OPERATOR <= (citext, citext) SET (
    RESTRICT   = scalarlesel,
    JOIN       = scalarlejoinsel
);

ALTER OPERATOR >= (citext, citext) SET (
    RESTRICT   = scalargesel,
    JOIN       = scalargejoinsel
);

CREATE FUNCTION citext_pattern_lt( citext, citext )
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION citext_pattern_le( citext, citext )
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION citext_pattern_gt( citext, citext )
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION citext_pattern_ge( citext, citext )
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR ~<~ (
    LEFTARG    = CITEXT,
    RIGHTARG   = CITEXT,
    NEGATOR    = ~>=~,
    COMMUTATOR = ~>~,
    PROCEDURE  = citext_pattern_lt,
    RESTRICT   = scalarltsel,
    JOIN       = scalarltjoinsel
);

CREATE OPERATOR ~<=~ (
    LEFTARG    = CITEXT,
    RIGHTARG   = CITEXT,
    NEGATOR    = ~>~,
    COMMUTATOR = ~>=~,
    PROCEDURE  = citext_pattern_le,
    RESTRICT   = scalarltsel,
    JOIN       = scalarltjoinsel
);

CREATE OPERATOR ~>=~ (
    LEFTARG    = CITEXT,
    RIGHTARG   = CITEXT,
    NEGATOR    = ~<~,
    COMMUTATOR = ~<=~,
    PROCEDURE  = citext_pattern_ge,
    RESTRICT   = scalargtsel,
    JOIN       = scalargtjoinsel
);

CREATE OPERATOR ~>~ (
    LEFTARG    = CITEXT,
    RIGHTARG   = CITEXT,
    NEGATOR    = ~<=~,
    COMMUTATOR = ~<~,
    PROCEDURE  = citext_pattern_gt,
    RESTRICT   = scalargtsel,
    JOIN       = scalargtjoinsel
);

CREATE FUNCTION citext_pattern_cmp(citext, citext)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR CLASS citext_pattern_ops
FOR TYPE CITEXT USING btree AS
    OPERATOR    1   ~<~  (citext, citext),
    OPERATOR    2   ~<=~ (citext, citext),
    OPERATOR    3   =    (citext, citext),
    OPERATOR    4   ~>=~ (citext, citext),
    OPERATOR    5   ~>~  (citext, citext),
    FUNCTION    1   citext_pattern_cmp(citext, citext);
