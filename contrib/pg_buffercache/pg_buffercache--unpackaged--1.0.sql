/* contrib/pg_buffercache/pg_buffercache--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_buffercache" to load this file. \quit

ALTER EXTENSION pg_buffercache ADD function pg_buffercache_pages();
ALTER EXTENSION pg_buffercache ADD view pg_buffercache;
