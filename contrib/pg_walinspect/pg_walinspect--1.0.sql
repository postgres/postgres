/* contrib/pg_walinspect/pg_walinspect--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_walinspect" to load this file. \quit

--
-- pg_get_wal_record_info()
--
CREATE FUNCTION pg_get_wal_record_info(IN in_lsn pg_lsn,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT fpi_length int4,
    OUT description text,
    OUT block_ref text
)
AS 'MODULE_PATHNAME', 'pg_get_wal_record_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn) TO pg_read_server_files;

--
-- pg_get_wal_records_info()
--
CREATE FUNCTION pg_get_wal_records_info(IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT fpi_length int4,
    OUT description text,
    OUT block_ref text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_records_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn) TO pg_read_server_files;

--
-- pg_get_wal_records_info_till_end_of_wal()
--
CREATE FUNCTION pg_get_wal_records_info_till_end_of_wal(IN start_lsn pg_lsn,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT fpi_length int4,
    OUT description text,
    OUT block_ref text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_records_info_till_end_of_wal'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info_till_end_of_wal(pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_records_info_till_end_of_wal(pg_lsn) TO pg_read_server_files;

--
-- pg_get_wal_stats()
--
CREATE FUNCTION pg_get_wal_stats(IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    IN  per_record boolean DEFAULT false,
    OUT "resource_manager/record_type" text,
    OUT count int8,
    OUT count_percentage float8,
    OUT record_size int8,
    OUT record_size_percentage float8,
    OUT fpi_size int8,
    OUT fpi_size_percentage float8,
    OUT combined_size int8,
    OUT combined_size_percentage float8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_stats'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean) TO pg_read_server_files;

--
-- pg_get_wal_stats_till_end_of_wal()
--
CREATE FUNCTION pg_get_wal_stats_till_end_of_wal(IN start_lsn pg_lsn,
    IN  per_record boolean DEFAULT false,
    OUT "resource_manager/record_type" text,
    OUT count int8,
    OUT count_percentage float8,
    OUT record_size int8,
    OUT record_size_percentage float8,
    OUT fpi_size int8,
    OUT fpi_size_percentage float8,
    OUT combined_size int8,
    OUT combined_size_percentage float8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_stats_till_end_of_wal'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_stats_till_end_of_wal(pg_lsn, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_stats_till_end_of_wal(pg_lsn, boolean) TO pg_read_server_files;
