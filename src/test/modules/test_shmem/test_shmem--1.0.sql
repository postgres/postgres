/* src/test/modules/test_shmem/test_shmem--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shmem" to load this file. \quit


CREATE FUNCTION get_test_shmem_attach_count()
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
