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
REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES ON deptest FROM regression_user;
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

\set VERBOSITY default
-- if we drop the object, we can drop the user too
DROP TABLE deptest;
DROP USER regression_user3;

-- Test DROP OWNED
CREATE USER regression_user0;
CREATE USER regression_user1;
CREATE USER regression_user2;
SET SESSION AUTHORIZATION regression_user0;
-- permission denied
DROP OWNED BY regression_user1;
DROP OWNED BY regression_user0, regression_user2;
REASSIGN OWNED BY regression_user0 TO regression_user1;
REASSIGN OWNED BY regression_user1 TO regression_user0;
-- this one is allowed
DROP OWNED BY regression_user0;

CREATE TABLE deptest1 (f1 int unique);
GRANT ALL ON deptest1 TO regression_user1 WITH GRANT OPTION;

SET SESSION AUTHORIZATION regression_user1;
CREATE TABLE deptest (a serial primary key, b text);
GRANT ALL ON deptest1 TO regression_user2;
RESET SESSION AUTHORIZATION;
\z deptest1

DROP OWNED BY regression_user1;
-- all grants revoked
\z deptest1
-- table was dropped
\d deptest

-- Test REASSIGN OWNED
GRANT ALL ON deptest1 TO regression_user1;

SET SESSION AUTHORIZATION regression_user1;
CREATE TABLE deptest (a serial primary key, b text);

CREATE TABLE deptest2 (f1 int);
-- make a serial column the hard way
CREATE SEQUENCE ss1;
ALTER TABLE deptest2 ALTER f1 SET DEFAULT nextval('ss1');
ALTER SEQUENCE ss1 OWNED BY deptest2.f1;
RESET SESSION AUTHORIZATION;

REASSIGN OWNED BY regression_user1 TO regression_user2;
\dt deptest

-- doesn't work: grant still exists
DROP USER regression_user1;
DROP OWNED BY regression_user1;
DROP USER regression_user1;

\set VERBOSITY terse
DROP USER regression_user2;
DROP OWNED BY regression_user2, regression_user0;
DROP USER regression_user2;
DROP USER regression_user0;
