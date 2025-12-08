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
-- injection_points_attach()
--
-- Attaches a function to the given injection point, with library name,
-- function name and private data.
--
CREATE FUNCTION injection_points_attach(IN point_name TEXT,
    IN library_name TEXT, IN function_name TEXT, IN private_data BYTEA)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_attach_func'
LANGUAGE C PARALLEL UNSAFE;

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
CREATE FUNCTION injection_points_run(IN point_name TEXT,
    IN arg TEXT DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_run'
LANGUAGE C PARALLEL UNSAFE;

--
-- injection_points_cached()
--
-- Executes the action attached to the injection point, from local cache.
--
CREATE FUNCTION injection_points_cached(IN point_name TEXT,
    IN arg TEXT DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'injection_points_cached'
LANGUAGE C PARALLEL UNSAFE;

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
-- injection_points_list()
--
-- List of all the injection points currently attached.
--
CREATE FUNCTION injection_points_list(OUT point_name text,
   OUT library text,
   OUT function text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'injection_points_list'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

--
-- regress_injection.c functions
--
CREATE FUNCTION removable_cutoff(rel regclass)
RETURNS xid8
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT;
