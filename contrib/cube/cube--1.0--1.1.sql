/* contrib/cube/cube--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION cube UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION distance_chebyshev(cube, cube)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION distance_taxicab(cube, cube)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION cube_coord(cube, int4)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION cube_coord_llur(cube, int4)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR -> (
	LEFTARG = cube, RIGHTARG = int, PROCEDURE = cube_coord
);

CREATE OPERATOR ~> (
	LEFTARG = cube, RIGHTARG = int, PROCEDURE = cube_coord_llur
);

CREATE OPERATOR <#> (
	LEFTARG = cube, RIGHTARG = cube, PROCEDURE = distance_taxicab,
	COMMUTATOR = '<#>'
);

CREATE OPERATOR <-> (
	LEFTARG = cube, RIGHTARG = cube, PROCEDURE = cube_distance,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <=> (
	LEFTARG = cube, RIGHTARG = cube, PROCEDURE = distance_chebyshev,
	COMMUTATOR = '<=>'
);

CREATE FUNCTION g_cube_distance (internal, cube, smallint, oid)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

ALTER OPERATOR FAMILY gist_cube_ops USING gist ADD
	OPERATOR	15	~> (cube, int) FOR ORDER BY float_ops,
	OPERATOR	16	<#> (cube, cube) FOR ORDER BY float_ops,
	OPERATOR	17	<-> (cube, cube) FOR ORDER BY float_ops,
	OPERATOR	18	<=> (cube, cube) FOR ORDER BY float_ops,
	FUNCTION	8	(cube, cube)	g_cube_distance (internal, cube, smallint, oid);
