\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.5'" to load this file. \quit

CREATE FUNCTION pg_buffercache_evict(IN int)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_buffercache_evict'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;
