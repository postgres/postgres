/* contrib/pgstattuple/pgstattuple--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgstattuple UPDATE TO '1.2'" to load this file. \quit

ALTER EXTENSION pgstattuple DROP FUNCTION pgstattuple(oid);
DROP FUNCTION pgstattuple(oid);

CREATE FUNCTION pgstattuple(IN reloid regclass,
    OUT table_len BIGINT,		-- physical table length in bytes
    OUT tuple_count BIGINT,		-- number of live tuples
    OUT tuple_len BIGINT,		-- total tuples length in bytes
    OUT tuple_percent FLOAT8,		-- live tuples in %
    OUT dead_tuple_count BIGINT,	-- number of dead tuples
    OUT dead_tuple_len BIGINT,		-- total dead tuples length in bytes
    OUT dead_tuple_percent FLOAT8,	-- dead tuples in %
    OUT free_space BIGINT,		-- free space in bytes
    OUT free_percent FLOAT8)		-- free space in %
AS 'MODULE_PATHNAME', 'pgstattuplebyid'
LANGUAGE C STRICT;

CREATE FUNCTION pgstatindex(IN relname regclass,
    OUT version INT,
    OUT tree_level INT,
    OUT index_size BIGINT,
    OUT root_block_no BIGINT,
    OUT internal_pages BIGINT,
    OUT leaf_pages BIGINT,
    OUT empty_pages BIGINT,
    OUT deleted_pages BIGINT,
    OUT avg_leaf_density FLOAT8,
    OUT leaf_fragmentation FLOAT8)
AS 'MODULE_PATHNAME', 'pgstatindexbyid'
LANGUAGE C STRICT;

CREATE FUNCTION pg_relpages(IN relname regclass)
RETURNS BIGINT
AS 'MODULE_PATHNAME', 'pg_relpagesbyid'
LANGUAGE C STRICT;
