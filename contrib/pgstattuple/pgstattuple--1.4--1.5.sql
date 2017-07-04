/* contrib/pgstattuple/pgstattuple--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgstattuple UPDATE TO '1.5'" to load this file. \quit

CREATE OR REPLACE FUNCTION pgstattuple(IN relname text,
    OUT table_len BIGINT,		-- physical table length in bytes
    OUT tuple_count BIGINT,		-- number of live tuples
    OUT tuple_len BIGINT,		-- total tuples length in bytes
    OUT tuple_percent FLOAT8,		-- live tuples in %
    OUT dead_tuple_count BIGINT,	-- number of dead tuples
    OUT dead_tuple_len BIGINT,		-- total dead tuples length in bytes
    OUT dead_tuple_percent FLOAT8,	-- dead tuples in %
    OUT free_space BIGINT,		-- free space in bytes
    OUT free_percent FLOAT8)		-- free space in %
AS 'MODULE_PATHNAME', 'pgstattuple_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstattuple(text) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstattuple(text) TO pg_stat_scan_tables;

CREATE OR REPLACE FUNCTION pgstatindex(IN relname text,
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
AS 'MODULE_PATHNAME', 'pgstatindex_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstatindex(text) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstatindex(text) TO pg_stat_scan_tables;

CREATE OR REPLACE FUNCTION pg_relpages(IN relname text)
RETURNS BIGINT
AS 'MODULE_PATHNAME', 'pg_relpages_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_relpages(text) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_relpages(text) TO pg_stat_scan_tables;

/* New stuff in 1.1 begins here */

CREATE OR REPLACE FUNCTION pgstatginindex(IN relname regclass,
    OUT version INT4,
    OUT pending_pages INT4,
    OUT pending_tuples BIGINT)
AS 'MODULE_PATHNAME', 'pgstatginindex_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstatginindex(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstatginindex(regclass) TO pg_stat_scan_tables;

/* New stuff in 1.2 begins here */

CREATE OR REPLACE FUNCTION pgstattuple(IN reloid regclass,
    OUT table_len BIGINT,		-- physical table length in bytes
    OUT tuple_count BIGINT,		-- number of live tuples
    OUT tuple_len BIGINT,		-- total tuples length in bytes
    OUT tuple_percent FLOAT8,		-- live tuples in %
    OUT dead_tuple_count BIGINT,	-- number of dead tuples
    OUT dead_tuple_len BIGINT,		-- total dead tuples length in bytes
    OUT dead_tuple_percent FLOAT8,	-- dead tuples in %
    OUT free_space BIGINT,		-- free space in bytes
    OUT free_percent FLOAT8)		-- free space in %
AS 'MODULE_PATHNAME', 'pgstattuplebyid_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstattuple(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstattuple(regclass) TO pg_stat_scan_tables;

CREATE OR REPLACE FUNCTION pgstatindex(IN relname regclass,
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
AS 'MODULE_PATHNAME', 'pgstatindexbyid_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstatindex(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstatindex(regclass) TO pg_stat_scan_tables;

CREATE OR REPLACE FUNCTION pg_relpages(IN relname regclass)
RETURNS BIGINT
AS 'MODULE_PATHNAME', 'pg_relpagesbyid_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_relpages(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_relpages(regclass) TO pg_stat_scan_tables;

/* New stuff in 1.3 begins here */

CREATE OR REPLACE FUNCTION pgstattuple_approx(IN reloid regclass,
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
AS 'MODULE_PATHNAME', 'pgstattuple_approx_v1_5'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstattuple_approx(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstattuple_approx(regclass) TO pg_stat_scan_tables;

/* New stuff in 1.5 begins here */

CREATE OR REPLACE FUNCTION pgstathashindex(IN relname regclass,
	OUT version INTEGER,
	OUT bucket_pages BIGINT,
	OUT overflow_pages BIGINT,
	OUT bitmap_pages BIGINT,
	OUT unused_pages BIGINT,
	OUT live_items BIGINT,
	OUT dead_items BIGINT,
	OUT free_percent FLOAT8)
AS 'MODULE_PATHNAME', 'pgstathashindex'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pgstathashindex(regclass) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pgstathashindex(regclass) TO pg_stat_scan_tables;
