/* contrib/pgstattuple/pgstattuple--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgstattuple FROM unpackaged" to load this file. \quit

ALTER EXTENSION pgstattuple ADD function pgstattuple(text);
ALTER EXTENSION pgstattuple ADD function pgstattuple(oid);
ALTER EXTENSION pgstattuple ADD function pgstatindex(text);
ALTER EXTENSION pgstattuple ADD function pg_relpages(text);
