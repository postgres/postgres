/* contrib/lo/lo--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION lo" to load this file. \quit

--
--	Create the data type ... now just a domain over OID
--

CREATE DOMAIN lo AS pg_catalog.oid;

--
-- For backwards compatibility, define a function named lo_oid.
--
-- The other functions that formerly existed are not needed because
-- the implicit casts between a domain and its underlying type handle them.
--
CREATE FUNCTION lo_oid(lo) RETURNS pg_catalog.oid AS
'SELECT $1::pg_catalog.oid' LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE;

-- This is used in triggers
CREATE FUNCTION lo_manage()
RETURNS pg_catalog.trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;
