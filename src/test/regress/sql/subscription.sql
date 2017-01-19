--
-- SUBSCRIPTION
--

CREATE ROLE regress_subscription_user LOGIN SUPERUSER;
SET SESSION AUTHORIZATION 'regress_subscription_user';

-- fail - no publications
CREATE SUBSCRIPTION testsub CONNECTION 'foo';

-- fail - no connection
CREATE SUBSCRIPTION testsub PUBLICATION foo;

set client_min_messages to error;
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

DROP SUBSCRIPTION testsub NODROP SLOT;

RESET SESSION AUTHORIZATION;
DROP ROLE regress_subscription_user;
