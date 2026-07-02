--
-- SUBSCRIPTION
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_fdw_connection(oid, oid, internal)
    RETURNS text
    AS :'regresslib', 'test_fdw_connection'
    LANGUAGE C;

CREATE ROLE regress_subscription_user LOGIN SUPERUSER;
CREATE ROLE regress_subscription_user2;
CREATE ROLE regress_subscription_user3 IN ROLE pg_create_subscription;
CREATE ROLE regress_subscription_user_dummy LOGIN NOSUPERUSER;
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - no publications
CREATE SUBSCRIPTION regress_testsub CONNECTION 'foo';

-- fail - no connection
CREATE SUBSCRIPTION regress_testsub PUBLICATION foo;

-- fail - cannot do CREATE SUBSCRIPTION CREATE SLOT inside transaction block
BEGIN;
CREATE SUBSCRIPTION regress_testsub CONNECTION 'testconn' PUBLICATION testpub WITH (create_slot);
COMMIT;

-- fail - invalid connection string
CREATE SUBSCRIPTION regress_testsub CONNECTION 'testconn' PUBLICATION testpub;

-- fail - duplicate publications
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION foo, testpub, foo WITH (connect = false);

-- ok
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

COMMENT ON SUBSCRIPTION regress_testsub IS 'test subscription';
SELECT obj_description(s.oid, 'pg_subscription') FROM pg_subscription s;

-- Check that only subconninfo is not publicly readable in pg_subscription.
SELECT count(*) = 0 AS ok
    FROM pg_attribute
    WHERE attrelid = 'pg_catalog.pg_subscription'::regclass AND attnum > 0 AND NOT attisdropped
        AND ((attname = 'subconninfo'
	        AND has_column_privilege('regress_subscription_user_dummy',
		    'pg_catalog.pg_subscription', attname, 'SELECT'))
            OR (attname <> 'subconninfo'
	        AND NOT has_column_privilege('regress_subscription_user_dummy',
		    'pg_catalog.pg_subscription', attname, 'SELECT')));

-- Check if the subscription stats are created and stats_reset is updated
-- by pg_stat_reset_subscription_stats().
SELECT subname, stats_reset IS NULL stats_reset_is_null FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';
SELECT pg_stat_reset_subscription_stats(oid) FROM pg_subscription WHERE subname = 'regress_testsub';
SELECT subname, stats_reset IS NULL stats_reset_is_null FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';

-- Reset the stats again and check if the new reset_stats is updated.
SELECT stats_reset as prev_stats_reset FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub' \gset
SELECT pg_stat_reset_subscription_stats(oid) FROM pg_subscription WHERE subname = 'regress_testsub';
SELECT :'prev_stats_reset' < stats_reset FROM pg_stat_subscription_stats WHERE subname = 'regress_testsub';

-- fail - name already exists
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail - must be superuser
SET SESSION AUTHORIZATION 'regress_subscription_user2';
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION foo WITH (connect = false);
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - invalid option combinations
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, copy_data = true);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, enabled = true);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, create_slot = true);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = true);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = false, create_slot = true);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = false);
CREATE SUBSCRIPTION regress_testsub2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, create_slot = false);

-- ok - with slot_name = NONE
CREATE SUBSCRIPTION regress_testsub3 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false);
-- fail
ALTER SUBSCRIPTION regress_testsub3 ENABLE;
ALTER SUBSCRIPTION regress_testsub3 REFRESH PUBLICATION;

-- fail - origin must be either none or any
CREATE SUBSCRIPTION regress_testsub4 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false, origin = foo);

-- now it works
CREATE SUBSCRIPTION regress_testsub4 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false, origin = none);
\dRs+ regress_testsub4
ALTER SUBSCRIPTION regress_testsub4 SET (origin = any);
\dRs+ regress_testsub4

DROP SUBSCRIPTION regress_testsub3;
DROP SUBSCRIPTION regress_testsub4;

-- fail, connection string does not parse
CREATE SUBSCRIPTION regress_testsub5 CONNECTION 'i_dont_exist=param' PUBLICATION testpub;

-- fail, connection string parses, but doesn't work (and does so without
-- connecting, so this is reliable and safe)
CREATE SUBSCRIPTION regress_testsub5 CONNECTION 'port=-1' PUBLICATION testpub;

CREATE FOREIGN DATA WRAPPER test_fdw;
CREATE SERVER test_server FOREIGN DATA WRAPPER test_fdw;

GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;

-- fail, need USAGE privileges on server
CREATE SUBSCRIPTION regress_testsub6 SERVER test_server PUBLICATION testpub WITH (slot_name = NONE, connect = false);

RESET SESSION AUTHORIZATION;
GRANT USAGE ON FOREIGN SERVER test_server TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;

-- fail, need user mapping
CREATE SUBSCRIPTION regress_testsub6 SERVER test_server PUBLICATION testpub WITH (slot_name = NONE, connect = false);

CREATE USER MAPPING FOR regress_subscription_user3 SERVER test_server OPTIONS(user 'foo', password 'secret');

-- fail, need CONNECTION clause
CREATE SUBSCRIPTION regress_testsub6 SERVER test_server PUBLICATION testpub WITH (slot_name = NONE, connect = false);

RESET SESSION AUTHORIZATION;
ALTER FOREIGN DATA WRAPPER test_fdw CONNECTION test_fdw_connection;
SET SESSION AUTHORIZATION regress_subscription_user3;

CREATE SUBSCRIPTION regress_testsub6 SERVER test_server
  PUBLICATION testpub WITH (slot_name = 'dummy', connect = false);

RESET SESSION AUTHORIZATION;
REVOKE USAGE ON FOREIGN SERVER test_server FROM regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;

-- ok, lacks USAGE on test_server, but replacing connection anyway
BEGIN;
ALTER SUBSCRIPTION regress_testsub6 CONNECTION 'dbname=regress_doesnotexist password=secret';
ABORT;

-- fails, cannot drop slot
DROP SUBSCRIPTION regress_testsub6;

RESET SESSION AUTHORIZATION;
GRANT USAGE ON FOREIGN SERVER test_server TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;

ALTER SUBSCRIPTION regress_testsub6 SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub6; --ok

CREATE SUBSCRIPTION regress_testsub6 SERVER test_server
  PUBLICATION testpub WITH (slot_name = 'dummy', connect = false);

DROP USER MAPPING FOR regress_subscription_user3 SERVER test_server;

-- ok, test_server lacks user mapping, but replacing connection anyway
BEGIN;
ALTER SUBSCRIPTION regress_testsub6 CONNECTION 'dbname=regress_doesnotexist password=secret';
ABORT;

-- fails, cannot drop slot
DROP SUBSCRIPTION regress_testsub6;

ALTER SUBSCRIPTION regress_testsub6 DISABLE;
ALTER SUBSCRIPTION regress_testsub6 SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub6; --ok

SET SESSION AUTHORIZATION regress_subscription_user;
REVOKE CREATE ON DATABASE REGRESSION FROM regress_subscription_user3;

DROP SERVER test_server;

-- fail, FDW is dependent
DROP FUNCTION test_fdw_connection(oid, oid, internal);
-- warn
ALTER FOREIGN DATA WRAPPER test_fdw NO CONNECTION;

DROP FUNCTION test_fdw_connection(oid, oid, internal);

DROP FOREIGN DATA WRAPPER test_fdw;

-- fail - invalid connection string during ALTER
ALTER SUBSCRIPTION regress_testsub CONNECTION 'foobar';

\dRs+

ALTER SUBSCRIPTION regress_testsub SET PUBLICATION testpub2, testpub3 WITH (refresh = false);
ALTER SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist2';
ALTER SUBSCRIPTION regress_testsub SET (slot_name = 'newname');
ALTER SUBSCRIPTION regress_testsub SET (password_required = false);
ALTER SUBSCRIPTION regress_testsub SET (run_as_owner = true);
\dRs+

ALTER SUBSCRIPTION regress_testsub SET (password_required = true);
ALTER SUBSCRIPTION regress_testsub SET (run_as_owner = false);

-- fail
ALTER SUBSCRIPTION regress_testsub SET (slot_name = '');

-- fail
ALTER SUBSCRIPTION regress_doesnotexist CONNECTION 'dbname=regress_doesnotexist2';
ALTER SUBSCRIPTION regress_testsub SET (create_slot = false);

-- ok
ALTER SUBSCRIPTION regress_testsub SKIP (lsn = '0/12345');

\dRs+

-- ok - with lsn = NONE
ALTER SUBSCRIPTION regress_testsub SKIP (lsn = NONE);

-- fail
ALTER SUBSCRIPTION regress_testsub SKIP (lsn = '0/0');

\dRs+

BEGIN;
ALTER SUBSCRIPTION regress_testsub ENABLE;

\dRs

ALTER SUBSCRIPTION regress_testsub DISABLE;

\dRs

COMMIT;

-- fail - must be owner of subscription
SET ROLE regress_subscription_user_dummy;
ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub_dummy;
RESET ROLE;

ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub_foo;
ALTER SUBSCRIPTION regress_testsub_foo SET (synchronous_commit = local);
ALTER SUBSCRIPTION regress_testsub_foo SET (synchronous_commit = foobar);
ALTER SUBSCRIPTION regress_testsub_foo SET (wal_receiver_timeout = '-1');
ALTER SUBSCRIPTION regress_testsub_foo SET (wal_receiver_timeout = '80s');
ALTER SUBSCRIPTION regress_testsub_foo SET (wal_receiver_timeout = 'foobar');

\dRs+

-- rename back to keep the rest simple
ALTER SUBSCRIPTION regress_testsub_foo RENAME TO regress_testsub;

-- ok, we're a superuser
ALTER SUBSCRIPTION regress_testsub OWNER TO regress_subscription_user2;

-- fail - cannot do DROP SUBSCRIPTION inside transaction block with slot name
BEGIN;
DROP SUBSCRIPTION regress_testsub;
COMMIT;

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

-- now it works
BEGIN;
DROP SUBSCRIPTION regress_testsub;
COMMIT;

DROP SUBSCRIPTION IF EXISTS regress_testsub;
DROP SUBSCRIPTION regress_testsub;  -- fail

-- fail - binary must be boolean
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, binary = foo);

-- now it works
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, binary = true);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (binary = false);
ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

\dRs+

DROP SUBSCRIPTION regress_testsub;

-- fail - streaming must be boolean or 'parallel'
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = foo);

-- now it works
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = true);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (streaming = parallel);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (streaming = false);
ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);

\dRs+

-- fail - publication already exists
ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub WITH (refresh = false);

-- fail - publication used more than once
ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub1 WITH (refresh = false);

-- ok - add two publications into subscription
ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub2 WITH (refresh = false);

-- fail - publications already exist
ALTER SUBSCRIPTION regress_testsub ADD PUBLICATION testpub1, testpub2 WITH (refresh = false);

\dRs+

-- fail - publication used more than once
ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub1, testpub1 WITH (refresh = false);

-- fail - all publications are deleted
ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub, testpub1, testpub2 WITH (refresh = false);

-- fail - publication does not exist in subscription
ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub3 WITH (refresh = false);

-- ok - delete publications
ALTER SUBSCRIPTION regress_testsub DROP PUBLICATION testpub1, testpub2 WITH (refresh = false);

\dRs+

DROP SUBSCRIPTION regress_testsub;

CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION mypub
       WITH (connect = false, create_slot = false, copy_data = false);

ALTER SUBSCRIPTION regress_testsub ENABLE;

-- fail - ALTER SUBSCRIPTION with refresh is not allowed in a transaction
-- block or function
BEGIN;
ALTER SUBSCRIPTION regress_testsub SET PUBLICATION mypub WITH (refresh = true);
END;

BEGIN;
ALTER SUBSCRIPTION regress_testsub REFRESH PUBLICATION;
END;

CREATE FUNCTION func() RETURNS VOID AS
$$ ALTER SUBSCRIPTION regress_testsub SET PUBLICATION mypub WITH (refresh = true) $$ LANGUAGE SQL;
SELECT func();

ALTER SUBSCRIPTION regress_testsub DISABLE;
ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;
DROP FUNCTION func;

-- fail - two_phase must be boolean
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, two_phase = foo);

-- now it works
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, two_phase = true);

\dRs+
-- we can alter streaming when two_phase enabled
ALTER SUBSCRIPTION regress_testsub SET (streaming = true);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

-- two_phase and streaming are compatible.
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, streaming = true, two_phase = true);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

-- fail - disable_on_error must be boolean
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, disable_on_error = foo);

-- now it works
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, disable_on_error = false);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (disable_on_error = true);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

-- fail - retain_dead_tuples must be boolean
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, retain_dead_tuples = foo);

-- ok
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, retain_dead_tuples = false);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

-- fail - max_retention_duration must be integer
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, max_retention_duration = foo);

-- fail - max_retention_duration must be non-negative
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, max_retention_duration = -1);

-- ok
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, max_retention_duration = 1000);

\dRs+

-- fail - max_retention_duration must be non-negative
ALTER SUBSCRIPTION regress_testsub SET (max_retention_duration = -1);

-- ok
ALTER SUBSCRIPTION regress_testsub SET (max_retention_duration = 0);

\dRs+

ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

-- let's do some tests with pg_create_subscription rather than superuser
SET SESSION AUTHORIZATION regress_subscription_user3;

-- fail, not enough privileges
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail, must specify password
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail, can't set password_required=false
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, password_required = false);

-- ok
RESET SESSION AUTHORIZATION;
GRANT CREATE ON DATABASE REGRESSION TO regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
CREATE SUBSCRIPTION regress_testsub CONNECTION 'dbname=regress_doesnotexist password=regress_fakepassword' PUBLICATION testpub WITH (connect = false);

-- we cannot give the subscription away to some random user
ALTER SUBSCRIPTION regress_testsub OWNER TO regress_subscription_user;

-- but we can rename the subscription we just created
ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub2;

-- ok, even after losing pg_create_subscription we can still rename it
RESET SESSION AUTHORIZATION;
REVOKE pg_create_subscription FROM regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
ALTER SUBSCRIPTION regress_testsub2 RENAME TO regress_testsub;

-- fail, after losing CREATE on the database we can't rename it any more
RESET SESSION AUTHORIZATION;
REVOKE CREATE ON DATABASE REGRESSION FROM regress_subscription_user3;
SET SESSION AUTHORIZATION regress_subscription_user3;
ALTER SUBSCRIPTION regress_testsub RENAME TO regress_testsub2;

-- fail - cannot do ALTER SUBSCRIPTION SET (failover) inside transaction block
BEGIN;
ALTER SUBSCRIPTION regress_testsub SET (failover);
COMMIT;

-- ok, owning it is enough for this stuff
ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

--
-- CONFLICT LOG DESTINATION TESTS
--

SET SESSION AUTHORIZATION 'regress_subscription_user';

SET client_min_messages = WARNING;

-- fail - unrecognized parameter value
CREATE SUBSCRIPTION regress_conflict_fail CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, conflict_log_destination = 'invalid');

-- verify subconflictlogdest is 'log' and subconflictlogrelid is 0 (InvalidOid) for default case
CREATE SUBSCRIPTION regress_conflict_log_default CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);
SELECT subname, subconflictlogdest, subconflictlogrelid
FROM pg_subscription WHERE subname = 'regress_conflict_log_default';

-- fail - empty string parameter value
CREATE SUBSCRIPTION regress_conflict_empty_str CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, conflict_log_destination = '');

-- this should generate a conflict log table named pg_conflict_log_$subid$
CREATE SUBSCRIPTION regress_conflict_test1 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, conflict_log_destination = 'table');

-- check metadata in pg_subscription: destination should be 'table' and subconflictlogrelid valid
SELECT subname, subconflictlogdest, subconflictlogrelid > 0 AS has_relid
FROM pg_subscription WHERE subname = 'regress_conflict_test1';

-- verify the physical table exists, its OID matches subconflictlogrelid,
-- and it is located in the 'pg_conflict' namespace
SELECT n.nspname, (c.oid = s.subconflictlogrelid) AS "oid_matches"
FROM pg_class c
JOIN pg_subscription s ON c.relname = 'pg_conflict_log_' || s.oid
JOIN pg_namespace n ON c.relnamespace = n.oid
WHERE s.subname = 'regress_conflict_test1';

-- check if the conflict log table has the correct schema
SELECT a.attnum, a.attname
FROM pg_attribute a
JOIN pg_class c ON a.attrelid = c.oid
JOIN pg_subscription s ON c.relname = 'pg_conflict_log_' || s.oid
WHERE s.subname = 'regress_conflict_test1' AND a.attnum > 0
    ORDER BY a.attnum;

-- Changing the subscription owner should also update the owner
-- of the associated conflict log table.
ALTER SUBSCRIPTION regress_conflict_test1 OWNER TO regress_subscription_user2;
SELECT pg_catalog.pg_get_userbyid(c.relowner) AS owner
FROM pg_catalog.pg_class c
JOIN pg_catalog.pg_subscription s
        ON c.relname = 'pg_conflict_log_' || s.oid
WHERE s.subname = 'regress_conflict_test1';

-- Verify that a non-superuser subscription owner can truncate,
-- delete from, and select from the associated conflict log table.
SET ROLE 'regress_subscription_user2';

SELECT format('%I.%I', n.nspname, c.relname) AS conflict_log_table
FROM pg_catalog.pg_class c
JOIN pg_catalog.pg_namespace n
	ON n.oid = c.relnamespace
JOIN pg_catalog.pg_subscription s
	ON c.relname = 'pg_conflict_log_' || s.oid
WHERE s.subname = 'regress_conflict_test1'
\gset

TRUNCATE TABLE :conflict_log_table;
DELETE FROM :conflict_log_table;
SELECT COUNT(*) FROM :conflict_log_table;

RESET ROLE;

-- Restore the original subscription owner.
ALTER SUBSCRIPTION regress_conflict_test1 OWNER TO regress_subscription_user;

--
-- ALTER SUBSCRIPTION - conflict_log_destination state transitions
--
-- These tests verify the transition logic between different logging
-- destinations, ensuring conflict log tables are created or dropped as
-- expected
--
-- transition from 'log' to 'all'
-- a new conflict log table should be created
CREATE SUBSCRIPTION regress_conflict_test2 CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false, conflict_log_destination = 'log');
ALTER SUBSCRIPTION regress_conflict_test2 SET (conflict_log_destination = 'all');

-- verify metadata after ALTER (destination should be 'all')
SELECT subname, subconflictlogdest, subconflictlogrelid > 0 AS has_relid
FROM pg_subscription WHERE subname = 'regress_conflict_test2';

-- transition from 'all' to 'table'
-- should NOT drop the table, only change destination string
SELECT subconflictlogrelid AS old_relid FROM pg_subscription WHERE subname = 'regress_conflict_test2' \gset
ALTER SUBSCRIPTION regress_conflict_test2 SET (conflict_log_destination = 'table');
SELECT subconflictlogdest, subconflictlogrelid = :old_relid AS relid_unchanged
FROM pg_subscription WHERE subname = 'regress_conflict_test2';

-- transition from 'table' to 'log'
-- should drop the table and clear subconflictlogrelid
ALTER SUBSCRIPTION regress_conflict_test2 SET (conflict_log_destination = 'log');
SELECT subconflictlogdest, subconflictlogrelid
FROM pg_subscription WHERE subname = 'regress_conflict_test2';

-- verify the physical table is gone
SELECT count(*)
FROM pg_class c
JOIN pg_subscription s ON c.relname = 'pg_conflict_log_' || s.oid
WHERE s.subname = 'regress_conflict_test2';

--
-- PUBLICATION: Verify conflict log tables are not publishable
--
-- pg_relation_is_publishable should return false for conflict log tables to
-- prevent them from being accidentally included in publications
--
SELECT n.nspname, pg_relation_is_publishable(c.oid)
FROM pg_class c
JOIN pg_namespace n ON c.relnamespace = n.oid
JOIN pg_subscription s ON s.subconflictlogrelid = c.oid
WHERE s.subname = 'regress_conflict_test1';

--
-- Table Protection and Lifecycle Management
--
-- These tests verify that:
-- Manual DROP TABLE is disallowed
-- DROP SUBSCRIPTION automatically reaps the table
--
-- re-enable table logging for verification
ALTER SUBSCRIPTION regress_conflict_test1 SET (conflict_log_destination = 'table');

-- The conflict log table name contains the subscription OID, which is
-- non-deterministic.  Capture it into a psql variable and report only the
-- SQLSTATE, so the expected output does not depend on the OID.

-- fail - drop table not allowed due to internal dependency
SET client_min_messages = NOTICE;
SELECT 'pg_conflict.pg_conflict_log_' || oid AS clt1
    FROM pg_subscription WHERE subname = 'regress_conflict_test1' \gset
\set VERBOSITY sqlstate
DROP TABLE :clt1;
\set VERBOSITY default

-- CLEANUP: DROP SUBSCRIPTION reaps the table
ALTER SUBSCRIPTION regress_conflict_test1 DISABLE;
ALTER SUBSCRIPTION regress_conflict_test1 SET (slot_name = NONE);

-- Verify the table OID for reap check
SELECT 'pg_conflict.pg_conflict_log_' || oid AS internal_tablename FROM pg_subscription WHERE subname = 'regress_conflict_test1' \gset

SET client_min_messages = WARNING;
DROP SUBSCRIPTION regress_conflict_test1;

-- should return NULL, meaning the conflict log table was reaped via dependency
SELECT to_regclass(:'internal_tablename');

--
-- Additional Namespace and Table Protection Tests
--

-- Setup: Ensure we have a subscription with a conflict log table
CREATE SUBSCRIPTION regress_conflict_protection_test CONNECTION 'dbname=regress_doesnotexist'
    PUBLICATION testpub WITH (connect = false, conflict_log_destination = 'table');

-- The conflict log table is system-managed; its name contains the
-- subscription OID, which is non-deterministic.  Capture the name into a psql
-- variable and report only the SQLSTATE for the operations that must be
-- rejected, so the expected output stays free of the dynamic OID.  Every
-- statement in the VERBOSITY-sqlstate block below must fail with 42809
-- (wrong_object_type), except adding the table to a publication, which fails
-- with 22023 (invalid_parameter_value).
SELECT 'pg_conflict.' || relname AS clt
    FROM pg_class c JOIN pg_subscription s ON c.relname = 'pg_conflict_log_' || s.oid
    WHERE s.subname = 'regress_conflict_protection_test' \gset

-- Trigger function used by the CREATE TRIGGER check below.
CREATE FUNCTION public.dummy_trigger_func() RETURNS trigger AS $$
BEGIN
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

\set VERBOSITY sqlstate
ALTER TABLE :clt ADD COLUMN extra_info text;
INSERT INTO :clt (relname) VALUES ('mytest');
UPDATE :clt SET relname = 'mytest';
CREATE POLICY p1 ON :clt USING (true);
CREATE STATISTICS s1 ON relname, schemaname FROM :clt;
CREATE TABLE public.conflict_child () INHERITS (:clt);
ALTER TABLE :clt RENAME COLUMN relname TO new_relname;
CREATE TABLE public.conflict_fk (relname text REFERENCES :clt(relname));
ALTER TABLE :clt OWNER TO regress_subscription_user_dummy;
ALTER TABLE :clt SET SCHEMA public;
CREATE TRIGGER t1 BEFORE INSERT ON :clt FOR EACH ROW EXECUTE FUNCTION public.dummy_trigger_func();
ALTER TRIGGER non_existent_trigger ON :clt RENAME TO new_trigger;
CREATE RULE r1 AS ON INSERT TO :clt DO INSTEAD NOTHING;
ALTER RULE non_existent_rule ON :clt RENAME TO new_rule;
CREATE INDEX idx1 ON :clt (relname);
SELECT 1 FROM :clt FOR UPDATE;
CREATE PUBLICATION testpub_for_clt FOR TABLE :clt;
\set VERBOSITY default

-- Clean up the trigger function used above.
DROP FUNCTION public.dummy_trigger_func();

-- TRUNCATE and DELETE are allowed so that users can prune the conflict log.
TRUNCATE :clt;
DELETE FROM :clt;

-- Creating a table directly in the pg_conflict namespace is rejected for
-- everyone (the schema is reserved for conflict log tables).
CREATE TABLE pg_conflict.manual_table (id int);

-- Moving a user table into the pg_conflict namespace is likewise rejected.
CREATE TABLE public.test_move (id int);
ALTER TABLE public.test_move SET SCHEMA pg_conflict;
DROP TABLE public.test_move;


SET client_min_messages = WARNING;

-- Clean up remaining test subscription
ALTER SUBSCRIPTION regress_conflict_log_default DISABLE;
ALTER SUBSCRIPTION regress_conflict_log_default SET (slot_name = NONE);
DROP SUBSCRIPTION regress_conflict_log_default;


ALTER SUBSCRIPTION regress_conflict_test2 DISABLE;
ALTER SUBSCRIPTION regress_conflict_test2 SET (slot_name = NONE);
DROP SUBSCRIPTION regress_conflict_test2;

ALTER SUBSCRIPTION regress_conflict_protection_test DISABLE;
ALTER SUBSCRIPTION regress_conflict_protection_test SET (slot_name = NONE);
DROP SUBSCRIPTION regress_conflict_protection_test;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user2;
DROP ROLE regress_subscription_user3;
DROP ROLE regress_subscription_user_dummy;
