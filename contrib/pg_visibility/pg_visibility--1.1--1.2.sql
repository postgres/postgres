/* contrib/pg_visibility/pg_visibility--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_visibility UPDATE TO '1.2'" to load this file. \quit

-- Allow use of monitoring functions by pg_monitor members
GRANT EXECUTE ON FUNCTION pg_visibility_map(regclass, bigint) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_visibility(regclass, bigint) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_visibility_map(regclass) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_visibility(regclass) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_visibility_map_summary(regclass) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_check_frozen(regclass) TO pg_stat_scan_tables;
GRANT EXECUTE ON FUNCTION pg_check_visible(regclass) TO pg_stat_scan_tables;
