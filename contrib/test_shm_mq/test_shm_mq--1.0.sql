/* contrib/test_shm_mq/test_shm_mq--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shm_mq" to load this file. \quit

CREATE FUNCTION test_shm_mq(queue_size pg_catalog.int8,
					   message pg_catalog.text,
					   repeat_count pg_catalog.int4 default 1,
					   num_workers pg_catalog.int4 default 1)
    RETURNS pg_catalog.void STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_shm_mq_pipelined(queue_size pg_catalog.int8,
					   message pg_catalog.text,
					   repeat_count pg_catalog.int4 default 1,
					   num_workers pg_catalog.int4 default 1,
					   verify pg_catalog.bool default true)
    RETURNS pg_catalog.void STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;
