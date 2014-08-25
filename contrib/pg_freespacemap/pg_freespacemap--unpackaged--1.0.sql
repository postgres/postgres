/* contrib/pg_freespacemap/pg_freespacemap--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_freespacemap FROM unpackaged" to load this file. \quit

ALTER EXTENSION pg_freespacemap ADD function pg_freespace(regclass,bigint);
ALTER EXTENSION pg_freespacemap ADD function pg_freespace(regclass);
