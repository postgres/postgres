/* $PostgreSQL: pgsql/contrib/pgstattuple/uninstall_pgstattuple.sql,v 1.6 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pgstattuple(text);
DROP FUNCTION pgstattuple(oid);
DROP FUNCTION pgstatindex(text);
DROP FUNCTION pg_relpages(text);
