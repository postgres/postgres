/* src/test/modules/xid_wraparound/xid_wraparound--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION xid_wraparound" to load this file. \quit

CREATE FUNCTION consume_xids(nxids bigint)
RETURNS xid8 VOLATILE PARALLEL UNSAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION consume_xids_until(targetxid xid8)
RETURNS xid8 VOLATILE PARALLEL UNSAFE STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
