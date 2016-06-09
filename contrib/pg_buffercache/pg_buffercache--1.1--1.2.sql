/* contrib/pg_buffercache/pg_buffercache--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.2'" to load this file. \quit

ALTER FUNCTION pg_buffercache_pages() PARALLEL SAFE;
