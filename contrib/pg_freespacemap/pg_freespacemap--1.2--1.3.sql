/* contrib/pg_freespacemap/pg_freespacemap--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_freespacemap UPDATE TO '1.3'" to load this file. \quit

CREATE OR REPLACE FUNCTION
  pg_freespace(rel regclass, blkno OUT bigint, avail OUT int2)
RETURNS SETOF RECORD
LANGUAGE SQL PARALLEL SAFE
BEGIN ATOMIC
  SELECT blkno, pg_freespace($1, blkno) AS avail
  FROM generate_series(0, pg_relation_size($1) / current_setting('block_size')::bigint - 1) AS blkno;
END;
