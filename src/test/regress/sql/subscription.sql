--
-- SUBSCRIPTION
--

CREATE ROLE regress_subscription_user LOGIN SUPERUSER;
CREATE ROLE regress_subscription_user2;
CREATE ROLE regress_subscription_user_dummy LOGIN NOSUPERUSER;
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - no publications
CREATE SUBSCRIPTION testsub CONNECTION 'foo';

-- fail - no connection
CREATE SUBSCRIPTION testsub PUBLICATION foo;

-- fail - cannot do CREATE SUBSCRIPTION CREATE SLOT inside transaction block
BEGIN;
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub WITH (create_slot);
COMMIT;

-- fail - invalid connection string
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub;

-- fail - duplicate publications
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION foo, testpub, foo WITH (connect = false);

-- ok
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (connect = false);

COMMENT ON SUBSCRIPTION testsub IS 'test subscription';
SELECT obj_description(s.oid, 'pg_subscription') FROM pg_subscription s;

-- fail - name already exists
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (connect = false);

-- fail - must be superuser
SET SESSION AUTHORIZATION 'regress_subscription_user2';
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION foo WITH (connect = false);
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - invalid option combinations
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (connect = false, copy_data = true);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (connect = false, enabled = true);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (connect = false, create_slot = true);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = true);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, create_slot = true);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, enabled = false);
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, create_slot = false);

-- ok - with slot_name = NONE
CREATE SUBSCRIPTION testsub3 CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (slot_name = NONE, connect = false);
-- fail
ALTER SUBSCRIPTION testsub3 ENABLE;
ALTER SUBSCRIPTION testsub3 REFRESH PUBLICATION;

DROP SUBSCRIPTION testsub3;

-- fail - invalid connection string
ALTER SUBSCRIPTION testsub CONNECTION 'foobar';

\dRs+

ALTER SUBSCRIPTION testsub SET PUBLICATION testpub2, testpub3 WITH (refresh = false);
ALTER SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist2';
ALTER SUBSCRIPTION testsub SET (slot_name = 'newname');

-- fail
ALTER SUBSCRIPTION doesnotexist CONNECTION 'dbname=doesnotexist2';
ALTER SUBSCRIPTION testsub SET (create_slot = false);

\dRs+

BEGIN;
ALTER SUBSCRIPTION testsub ENABLE;

\dRs

ALTER SUBSCRIPTION testsub DISABLE;

\dRs

COMMIT;

-- fail - must be owner of subscription
SET ROLE regress_subscription_user_dummy;
ALTER SUBSCRIPTION testsub RENAME TO testsub_dummy;
RESET ROLE;

ALTER SUBSCRIPTION testsub RENAME TO testsub_foo;
ALTER SUBSCRIPTION testsub_foo SET (synchronous_commit = local);
ALTER SUBSCRIPTION testsub_foo SET (synchronous_commit = foobar);

\dRs+

-- rename back to keep the rest simple
ALTER SUBSCRIPTION testsub_foo RENAME TO testsub;

-- fail - new owner must be superuser
ALTER SUBSCRIPTION testsub OWNER TO regress_subscription_user2;
ALTER ROLE regress_subscription_user2 SUPERUSER;
-- now it works
ALTER SUBSCRIPTION testsub OWNER TO regress_subscription_user2;

-- fail - cannot do DROP SUBSCRIPTION inside transaction block with slot name
BEGIN;
DROP SUBSCRIPTION testsub;
COMMIT;

ALTER SUBSCRIPTION testsub SET (slot_name = NONE);

-- now it works
BEGIN;
DROP SUBSCRIPTION testsub;
COMMIT;

DROP SUBSCRIPTION IF EXISTS testsub;
DROP SUBSCRIPTION testsub;  -- fail

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user2;
DROP ROLE regress_subscription_user_dummy;
