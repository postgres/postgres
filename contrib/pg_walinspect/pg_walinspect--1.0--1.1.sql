/* contrib/pg_walinspect/pg_walinspect--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_walinspect UPDATE TO '1.1'" to load this file. \quit

-- Unsupported functions after 1.1.
DROP FUNCTION pg_get_wal_records_info_till_end_of_wal(pg_lsn);
DROP FUNCTION pg_get_wal_stats_till_end_of_wal(pg_lsn, boolean);

--
-- pg_get_wal_block_info()
--
CREATE FUNCTION pg_get_wal_block_info(IN start_lsn pg_lsn,
	IN end_lsn pg_lsn,
	IN show_data boolean DEFAULT true,
	OUT start_lsn pg_lsn,
	OUT end_lsn pg_lsn,
	OUT prev_lsn pg_lsn,
	OUT block_id int2,
	OUT reltablespace oid,
	OUT reldatabase oid,
	OUT relfilenode oid,
	OUT relforknumber int2,
	OUT relblocknumber int8,
	OUT xid xid,
	OUT resource_manager text,
	OUT record_type text,
	OUT record_length int4,
	OUT main_data_length int4,
	OUT block_data_length int4,
	OUT block_fpi_length int4,
	OUT block_fpi_info text[],
	OUT description text,
	OUT block_data bytea,
	OUT block_fpi_data bytea
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_block_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) TO pg_read_server_files;
