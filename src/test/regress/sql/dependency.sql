--
-- DEPENDENCIES
--

CREATE USER regression_user;
CREATE USER regression_user2;
CREATE USER regression_user3;
CREATE GROUP regression_group;

CREATE TABLE deptest (f1 serial primary key, f2 text);

GRANT SELECT ON TABLE deptest TO GROUP regression_group;
GRANT ALL ON TABLE deptest TO regression_user, regression_user2;

-- can't drop neither because they have privileges somewhere
DROP USER regression_user;
DROP GROUP regression_group;

-- if we revoke the privileges we can drop the group
REVOKE SELECT ON deptest FROM GROUP regression_group;
DROP GROUP regression_group;

-- can't drop the user if we revoke the privileges partially
REVOKE SELECT, INSERT, UPDATE, DELETE, RULE, REFERENCES ON deptest FROM regression_user;
DROP USER regression_user;

-- now we are OK to drop him
REVOKE TRIGGER ON deptest FROM regression_user;
DROP USER regression_user;

-- we are OK too if we drop the privileges all at once
REVOKE ALL ON deptest FROM regression_user2;
DROP USER regression_user2;

-- can't drop the owner of an object
-- the error message detail here would include a pg_toast_nnn name that
-- is not constant, so suppress it
\set VERBOSITY terse
ALTER TABLE deptest OWNER TO regression_user3;
DROP USER regression_user3;

-- if we drop the object, we can drop the user too
DROP TABLE deptest;
DROP USER regression_user3;
