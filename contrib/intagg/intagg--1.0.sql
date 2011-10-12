/* contrib/intagg/intagg--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION intagg" to load this file. \quit

-- Internal function for the aggregate
-- Is called for each item in an aggregation
CREATE FUNCTION int_agg_state (internal, int4)
RETURNS internal
AS 'array_agg_transfn'
LANGUAGE INTERNAL;

-- Internal function for the aggregate
-- Is called at the end of the aggregation, and returns an array.
CREATE FUNCTION int_agg_final_array (internal)
RETURNS int4[]
AS 'array_agg_finalfn'
LANGUAGE INTERNAL;

-- The aggregate function itself
-- uses the above functions to create an array of integers from an aggregation.
CREATE AGGREGATE int_array_aggregate (
	BASETYPE = int4,
	SFUNC = int_agg_state,
	STYPE = internal,
	FINALFUNC = int_agg_final_array
);

-- The enumeration function
-- returns each element in a one dimensional integer array
-- as a row.
CREATE FUNCTION int_array_enum(int4[])
RETURNS setof integer
AS 'array_unnest'
LANGUAGE INTERNAL IMMUTABLE STRICT;
