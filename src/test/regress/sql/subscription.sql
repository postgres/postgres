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

set client_min_messages to error;
-- fail - cannot do CREATE SUBSCRIPTION CREATE SLOT inside transaction block
BEGIN;
CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub WITH (CREATE SLOT);
COMMIT;

CREATE SUBSCRIPTION testsub CONNECTION 'testconn' PUBLICATION testpub;
CREATE SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist' PUBLICATION testpub WITH (DISABLED, NOCREATE SLOT);
reset client_min_messages;

\dRs+

ALTER SUBSCRIPTION testsub SET PUBLICATION testpub2, testpub3;

\dRs

ALTER SUBSCRIPTION testsub CONNECTION 'dbname=doesnotexist2';
ALTER SUBSCRIPTION testsub SET PUBLICATION testpub, testpub1;

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

-- fail - cannot do DROP SUBSCRIPTION DROP SLOT inside transaction block
BEGIN;
DROP SUBSCRIPTION testsub DROP SLOT;
COMMIT;

BEGIN;
DROP SUBSCRIPTION testsub_foo NODROP SLOT;
COMMIT;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
DROP ROLE regress_subscription_user_dummy;
