/* contrib/old_snapshot/old_snapshot--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION old_snapshot" to load this file. \quit

-- Show visibility map and page-level visibility information for each block.
CREATE FUNCTION pg_old_snapshot_time_mapping(array_offset OUT int4,
											 end_timestamp OUT timestamptz,
											 newest_xmin OUT xid)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_old_snapshot_time_mapping'
LANGUAGE C STRICT;

-- XXX. Do we want REVOKE commands here?
