--
-- SUBSCRIPTION
--

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
--fail - alter of two_phase option not supported.
ALTER SUBSCRIPTION regress_testsub SET (two_phase = false);

-- but can alter streaming when two_phase enabled
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

-- ok, owning it is enough for this stuff
ALTER SUBSCRIPTION regress_testsub SET (slot_name = NONE);
DROP SUBSCRIPTION regress_testsub;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user2;
DROP ROLE regress_subscription_user3;
DROP ROLE regress_subscription_user_dummy;
