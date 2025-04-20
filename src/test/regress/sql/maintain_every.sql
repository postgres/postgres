-- Test maintenance commands that visit every eligible relation.  Run as a
-- non-superuser, to skip other users' tables.

CREATE ROLE regress_maintain;
SET ROLE regress_maintain;

-- Test database-wide ANALYZE ("use_own_xacts" mode) setting relhassubclass=f
-- for non-partitioning inheritance, w/ ON COMMIT DELETE ROWS building an
-- empty index.
CREATE TEMP TABLE past_inh_db_other (); -- need 2 tables for "use_own_xacts"
CREATE TEMP TABLE past_inh_db_parent () ON COMMIT DELETE ROWS;
CREATE TEMP TABLE past_inh_db_child () INHERITS (past_inh_db_parent);
CREATE INDEX ON past_inh_db_parent ((1));
ANALYZE past_inh_db_parent;
SELECT reltuples, relhassubclass
  FROM pg_class WHERE oid = 'past_inh_db_parent'::regclass;
DROP TABLE past_inh_db_child;
SET client_min_messages = error; -- hide WARNINGs for other users' tables
ANALYZE;
RESET client_min_messages;
SELECT reltuples, relhassubclass
  FROM pg_class WHERE oid = 'past_inh_db_parent'::regclass;
DROP TABLE past_inh_db_parent, past_inh_db_other;

RESET ROLE;
DROP ROLE regress_maintain;
