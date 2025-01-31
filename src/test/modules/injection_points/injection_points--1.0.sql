/* src/test/modules/injection_points/injection_points--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION injection_points" to load this file. \quit

--
-- injection_points_attach()
--
-- Attaches the action to the given injection point.
--
CREATE FUNCTION injection_points_attach(IN point_name TEXT,
    IN action text)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_attach'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_load()
--
-- Load an injection point already attached.
--
CREATE FUNCTION injection_points_load(IN point_name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_load'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_run()
--
-- Executes the action attached to the injection point.
--
CREATE FUNCTION injection_points_run(IN point_name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_run'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_cached()
--
-- Executes the action attached to the injection point, from local cache.
--
CREATE FUNCTION injection_points_cached(IN point_name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_cached'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_wakeup()
--
-- Wakes up a waiting injection point.
--
CREATE FUNCTION injection_points_wakeup(IN point_name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_wakeup'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_set_local()
--
-- Trigger switch to link any future injection points attached to the
-- current process, useful to make SQL tests concurrently-safe.
--
CREATE FUNCTION injection_points_set_local()
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_set_local'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_detach()
--
-- Detaches the current action, if any, from the given injection point.
--
CREATE FUNCTION injection_points_detach(IN point_name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_detach'
LANGUAGE C STRICT PARALLEL UNSAFE;

--
-- injection_points_stats_numcalls()
--
-- Reports statistics, if any, related to the given injection point.
--
CREATE FUNCTION injection_points_stats_numcalls(IN point_name TEXT)
RETURNS bigint
AS 'MODULE_PATHNAME', 'injection_points_stats_numcalls'
LANGUAGE C STRICT;

--
-- injection_points_stats_drop()
--
-- Drop all statistics of injection points.
--
CREATE FUNCTION injection_points_stats_drop()
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_stats_drop'
LANGUAGE C STRICT;

--
-- injection_points_stats_fixed()
--
-- Reports fixed-numbered statistics for injection points.
CREATE FUNCTION injection_points_stats_fixed(OUT numattach int8,
   OUT numdetach int8,
   OUT numrun int8,
   OUT numcached int8,
   OUT numloaded int8)
RETURNS record
AS 'MODULE_PATHNAME', 'injection_points_stats_fixed'
LANGUAGE C STRICT;

--
-- regress_injection.c functions
--
CREATE FUNCTION removable_cutoff(rel regclass)
RETURNS xid8
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT;
