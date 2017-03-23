--
-- SUBSCRIPTION
--

CREATE ROLE regress_subscription_user LOGIN SUPERUSER;
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

CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub;

CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (NOCONNECT);

\dRs+

ALTER SUBSCRIPTION testsub SET PUBLICATION testpub2, testpub3 NOREFRESH;
ALTER SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist2';

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

-- fail - cannot do DROP SUBSCRIPTION DROP SLOT inside transaction block
BEGIN;
DROP SUBSCRIPTION testsub DROP SLOT;
COMMIT;

BEGIN;
DROP SUBSCRIPTION testsub NODROP SLOT;
COMMIT;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user_dummy;
