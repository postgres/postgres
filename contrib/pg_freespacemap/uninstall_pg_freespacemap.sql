/* contrib/pg_freespacemap/uninstall_pg_freespacemap.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pg_freespace(regclass, bigint);
DROP FUNCTION pg_freespace(regclass);
