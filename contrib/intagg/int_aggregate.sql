/* $PostgreSQL: pgsql/contrib/intagg/int_aggregate.sql,v 1.1 2008/11/14 19:58:45 tgl Exp $ */

-- Adjust this setting to control where the objects get created.
SET search_path = public;

-- Internal function for the aggregate
-- Is called for each item in an aggregation
CREATE OR REPLACE FUNCTION int_agg_state (internal, int4)
RETURNS internal
AS 'array_agg_transfn'
LANGUAGE INTERNAL;

-- Internal function for the aggregate
-- Is called at the end of the aggregation, and returns an array.
CREATE OR REPLACE FUNCTION int_agg_final_array (internal)
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
CREATE OR REPLACE FUNCTION int_array_enum(int4[])
RETURNS setof integer
AS 'array_unnest'
LANGUAGE INTERNAL IMMUTABLE STRICT;
