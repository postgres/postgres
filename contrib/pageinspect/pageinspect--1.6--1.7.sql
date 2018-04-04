/* contrib/pageinspect/pageinspect--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.7'" to load this file. \quit

--
-- bt_metap()
--
DROP FUNCTION bt_metap(IN relname text,
    OUT magic int4,
    OUT version int4,
    OUT root int4,
    OUT level int4,
    OUT fastroot int4,
    OUT fastlevel int4);
CREATE FUNCTION bt_metap(IN relname text,
    OUT magic int4,
    OUT version int4,
    OUT root int4,
    OUT level int4,
    OUT fastroot int4,
    OUT fastlevel int4,
    OUT oldest_xact int4,
    OUT last_cleanup_num_tuples real)
AS 'MODULE_PATHNAME', 'bt_metap'
LANGUAGE C STRICT PARALLEL SAFE;
