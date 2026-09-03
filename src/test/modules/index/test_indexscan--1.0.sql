/* src/test/modules/index/test_indexscan--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_indexscan" to load this file. \quit

-- Scan indexrel with zero scan keys and the given snapshot type ('mvcc',
-- 'any', 'self', 'dirty', 'nonvacuumable'), in the forward direction unless
-- forwardscan is false, returning the heap TID of every tuple the scan
-- yields, in scan order
CREATE FUNCTION index_scan_tids(indexrel regclass, snaptype text,
                                forwardscan bool DEFAULT true)
RETURNS SETOF pg_catalog.tid
STRICT AS 'MODULE_PATHNAME' LANGUAGE C;
