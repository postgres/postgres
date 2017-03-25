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
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub WITH (CREATE SLOT);
COMMIT;

-- fail - invalid connection string
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub;

-- fail - duplicate publications
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION foo, testpub, foo WITH (NOCONNECT);

-- ok
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (NOCONNECT);

COMMENT ON SUBSCRIPTION testsub IS 'test subscription';
SELECT obj_description(s.oid, 'pg_subscription') FROM pg_subscription s;

-- fail - name already exists
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (NOCONNECT);

-- fail - must be superuser
SET SESSION AUTHORIZATION 'regress_subscription_user2';
CREATE SUBSCRIPTION testsub2 CONNECTION 'dbname=doesnotexist' PUBLICATION foo WITH (NOCONNECT);
SET SESSION AUTHORIZATION 'regress_subscription_user';

\dRs+

ALTER SUBSCRIPTION testsub SET PUBLICATION testpub2, testpub3 NOREFRESH;
ALTER SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist2';
ALTER SUBSCRIPTION testsub WITH (SLOT NAME = 'newname');

-- fail
ALTER SUBSCRIPTION doesnotexist CONNECTION 'dbname=doesnotexist2';

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

\dRs

-- rename back to keep the rest simple
ALTER SUBSCRIPTION testsub_foo RENAME TO testsub;

-- fail - new owner must be superuser
ALTER SUBSCRIPTION testsub OWNER TO regress_subscription_user2;
ALTER ROLE regress_subscription_user2 SUPERUSER;
-- now it works
ALTER SUBSCRIPTION testsub OWNER TO regress_subscription_user2;

-- fail - cannot do DROP SUBSCRIPTION DROP SLOT inside transaction block
BEGIN;
DROP SUBSCRIPTION testsub DROP SLOT;
COMMIT;

BEGIN;
DROP SUBSCRIPTION testsub NODROP SLOT;
COMMIT;

DROP SUBSCRIPTION IF EXISTS testsub NODROP SLOT;
DROP SUBSCRIPTION testsub NODROP SLOT;  -- fail

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user2;
DROP ROLE regress_subscription_user_dummy;
