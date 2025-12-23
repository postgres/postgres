/* contrib/pg_buffercache/pg_buffercache--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.7'" to load this file. \quit

-- Function to retrieve information about OS pages, with optional NUMA
-- information.
CREATE FUNCTION pg_buffercache_os_pages(IN include_numa boolean,
    OUT bufferid integer,
    OUT os_page_num bigint,
    OUT numa_node integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_buffercache_os_pages'
LANGUAGE C PARALLEL SAFE;

-- View for OS page information, without NUMA.
CREATE VIEW pg_buffercache_os_pages AS
    SELECT bufferid, os_page_num
    FROM pg_buffercache_os_pages(false);

-- Re-create view for OS page information, with NUMA.
DROP VIEW pg_buffercache_numa;
CREATE VIEW pg_buffercache_numa AS
    SELECT bufferid, os_page_num, numa_node
    FROM pg_buffercache_os_pages(true);

REVOKE ALL ON FUNCTION pg_buffercache_os_pages(boolean) FROM PUBLIC;
REVOKE ALL ON pg_buffercache_os_pages FROM PUBLIC;
REVOKE ALL ON pg_buffercache_numa FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_buffercache_os_pages(boolean) TO pg_monitor;
GRANT SELECT ON pg_buffercache_os_pages TO pg_monitor;
GRANT SELECT ON pg_buffercache_numa TO pg_monitor;

-- Functions to mark buffers as dirty.
CREATE FUNCTION pg_buffercache_mark_dirty(
    IN int,
    OUT buffer_dirtied boolean,
    OUT buffer_already_dirty boolean)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_mark_dirty_relation(
    IN regclass,
    OUT buffers_dirtied int4,
    OUT buffers_already_dirty int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty_relation'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_mark_dirty_all(
    OUT buffers_dirtied int4,
    OUT buffers_already_dirty int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty_all'
LANGUAGE C PARALLEL SAFE VOLATILE;
