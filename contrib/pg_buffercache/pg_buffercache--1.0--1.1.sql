/* contrib/pg_buffercache/pg_buffercache--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.1'" to load this file. \quit

-- Upgrade view to 1.1. format
CREATE OR REPLACE VIEW pg_buffercache AS
	SELECT P.* FROM pg_buffercache_pages() AS P
	(bufferid integer, relfilenode oid, reltablespace oid, reldatabase oid,
	 relforknumber int2, relblocknumber int8, isdirty bool, usagecount int2,
	 pinning_backends int4);
