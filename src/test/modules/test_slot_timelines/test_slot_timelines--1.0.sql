-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_slot_timelines" to load this file. \quit

CREATE OR REPLACE FUNCTION test_slot_timelines_create_logical_slot(slot_name text, plugin text)
RETURNS void
STRICT LANGUAGE c AS 'MODULE_PATHNAME';

COMMENT ON FUNCTION test_slot_timelines_create_logical_slot(text, text)
IS 'Create a logical slot at a particular lsn and xid. Do not use in production servers, it is not safe. The slot is created with an invalid xmin and lsn.';

CREATE OR REPLACE FUNCTION test_slot_timelines_advance_logical_slot(slot_name text, new_xmin xid, new_catalog_xmin xid, new_restart_lsn pg_lsn, new_confirmed_lsn pg_lsn)
RETURNS void
STRICT LANGUAGE c AS 'MODULE_PATHNAME';

COMMENT ON FUNCTION test_slot_timelines_advance_logical_slot(text, xid, xid, pg_lsn, pg_lsn)
IS 'Advance a logical slot directly. Do not use this in production servers, it is not safe.';
