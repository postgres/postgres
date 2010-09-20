/* contrib/pgrowlocks/uninstall_pgrowlocks.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pgrowlocks(text);
