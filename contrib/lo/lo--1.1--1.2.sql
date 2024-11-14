/* contrib/lo/lo--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION lo UPDATE TO '1.2'" to load this file. \quit

CREATE OR REPLACE FUNCTION lo_oid(lo) RETURNS pg_catalog.oid
LANGUAGE SQL STRICT IMMUTABLE PARALLEL SAFE
RETURN (SELECT $1::pg_catalog.oid);
