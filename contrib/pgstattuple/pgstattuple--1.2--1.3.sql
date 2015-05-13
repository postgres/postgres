/* contrib/pgstattuple/pgstattuple--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgstattuple UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION pgstattuple_approx(IN reloid regclass,
    OUT table_len BIGINT,               -- physical table length in bytes
    OUT scanned_percent FLOAT8,         -- what percentage of the table's pages was scanned
    OUT approx_tuple_count BIGINT,      -- estimated number of live tuples
    OUT approx_tuple_len BIGINT,        -- estimated total length in bytes of live tuples
    OUT approx_tuple_percent FLOAT8,    -- live tuples in % (based on estimate)
    OUT dead_tuple_count BIGINT,        -- exact number of dead tuples
    OUT dead_tuple_len BIGINT,          -- exact total length in bytes of dead tuples
    OUT dead_tuple_percent FLOAT8,      -- dead tuples in % (based on estimate)
    OUT approx_free_space BIGINT,       -- estimated free space in bytes
    OUT approx_free_percent FLOAT8)     -- free space in % (based on estimate)
AS 'MODULE_PATHNAME', 'pgstattuple_approx'
LANGUAGE C STRICT;
