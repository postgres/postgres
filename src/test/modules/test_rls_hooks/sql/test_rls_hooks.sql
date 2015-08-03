LOAD 'test_rls_hooks';

CREATE TABLE rls_test_permissive (
    username        name,
    supervisor      name,
    data            integer
);

-- initial test data
INSERT INTO rls_test_permissive VALUES ('r1','s1',4);
INSERT INTO rls_test_permissive VALUES ('r2','s2',5);
INSERT INTO rls_test_permissive VALUES ('r3','s3',6);

CREATE TABLE rls_test_restrictive (
    username        name,
    supervisor      name,
    data            integer
);

-- At least one permissive policy must exist, otherwise
-- the default deny policy will be applied.  For
-- testing the only-restrictive-policies from the hook,
-- create a simple 'allow all' policy.
CREATE POLICY p1 ON rls_test_restrictive USING (true);

-- initial test data
INSERT INTO rls_test_restrictive VALUES ('r1','s1',1);
INSERT INTO rls_test_restrictive VALUES ('r2','s2',2);
INSERT INTO rls_test_restrictive VALUES ('r3','s3',3);

CREATE TABLE rls_test_both (
    username        name,
    supervisor      name,
    data            integer
);

-- initial test data
INSERT INTO rls_test_both VALUES ('r1','s1',7);
INSERT INTO rls_test_both VALUES ('r2','s2',8);
INSERT INTO rls_test_both VALUES ('r3','s3',9);

ALTER TABLE rls_test_permissive ENABLE ROW LEVEL SECURITY;
ALTER TABLE rls_test_restrictive ENABLE ROW LEVEL SECURITY;
ALTER TABLE rls_test_both ENABLE ROW LEVEL SECURITY;

CREATE ROLE r1;
CREATE ROLE s1;

GRANT SELECT,INSERT ON rls_test_permissive TO r1;
GRANT SELECT,INSERT ON rls_test_restrictive TO r1;
GRANT SELECT,INSERT ON rls_test_both TO r1;

GRANT SELECT,INSERT ON rls_test_permissive TO s1;
GRANT SELECT,INSERT ON rls_test_restrictive TO s1;
GRANT SELECT,INSERT ON rls_test_both TO s1;

SET ROLE r1;

-- With only the hook's policies, permissive
-- hook's policy is current_user = username
EXPLAIN (costs off) SELECT * FROM rls_test_permissive;

SELECT * FROM rls_test_permissive;

-- success
INSERT INTO rls_test_permissive VALUES ('r1','s1',10);

-- failure
INSERT INTO rls_test_permissive VALUES ('r4','s4',10);

SET ROLE s1;

-- With only the hook's policies, restrictive
-- hook's policy is current_user = supervisor
EXPLAIN (costs off) SELECT * FROM rls_test_restrictive;

SELECT * FROM rls_test_restrictive;

-- success
INSERT INTO rls_test_restrictive VALUES ('r1','s1',10);

-- failure
INSERT INTO rls_test_restrictive VALUES ('r4','s4',10);

SET ROLE s1;

-- With only the hook's policies, both
-- permissive hook's policy is current_user = username
-- restrictive hook's policy is current_user = superuser
-- combined with AND, results in nothing being allowed
EXPLAIN (costs off) SELECT * FROM rls_test_both;

SELECT * FROM rls_test_both;

-- failure
INSERT INTO rls_test_both VALUES ('r1','s1',10);

-- failure
INSERT INTO rls_test_both VALUES ('r4','s1',10);

-- failure
INSERT INTO rls_test_both VALUES ('r4','s4',10);

RESET ROLE;

-- Create "internal" policies, to check that the policies from
-- the hooks are combined correctly.
CREATE POLICY p1 ON rls_test_permissive USING (data % 2 = 0);

-- Remove the original allow-all policy
DROP POLICY p1 ON rls_test_restrictive;
CREATE POLICY p1 ON rls_test_restrictive USING (data % 2 = 0);

CREATE POLICY p1 ON rls_test_both USING (data % 2 = 0);

SET ROLE r1;

-- With both internal and hook policies, permissive
EXPLAIN (costs off) SELECT * FROM rls_test_permissive;

SELECT * FROM rls_test_permissive;

-- success
INSERT INTO rls_test_permissive VALUES ('r1','s1',7);

-- success
INSERT INTO rls_test_permissive VALUES ('r3','s3',10);

-- failure
INSERT INTO rls_test_permissive VALUES ('r4','s4',7);

SET ROLE s1;

-- With both internal and hook policies, restrictive
EXPLAIN (costs off) SELECT * FROM rls_test_restrictive;

SELECT * FROM rls_test_restrictive;

-- success
INSERT INTO rls_test_restrictive VALUES ('r1','s1',8);

-- failure
INSERT INTO rls_test_restrictive VALUES ('r3','s3',10);

-- failure
INSERT INTO rls_test_restrictive VALUES ('r1','s1',7);

-- failure
INSERT INTO rls_test_restrictive VALUES ('r4','s4',7);

-- With both internal and hook policies, both permissive
-- and restrictive hook policies
EXPLAIN (costs off) SELECT * FROM rls_test_both;

SELECT * FROM rls_test_both;

-- success
INSERT INTO rls_test_both VALUES ('r1','s1',8);

-- failure
INSERT INTO rls_test_both VALUES ('r3','s3',10);

-- failure
INSERT INTO rls_test_both VALUES ('r1','s1',7);

-- failure
INSERT INTO rls_test_both VALUES ('r4','s4',7);

RESET ROLE;

DROP TABLE rls_test_restrictive;
DROP TABLE rls_test_permissive;
DROP TABLE rls_test_both;

DROP ROLE r1;
DROP ROLE s1;
