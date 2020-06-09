--
-- Test access privileges
--

-- Clean up in case a prior regression run failed

-- Suppress NOTICE messages when users/groups don't exist
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS regress_priv_group1;
DROP ROLE IF EXISTS regress_priv_group2;

DROP ROLE IF EXISTS regress_priv_user1;
DROP ROLE IF EXISTS regress_priv_user2;
DROP ROLE IF EXISTS regress_priv_user3;
DROP ROLE IF EXISTS regress_priv_user4;
DROP ROLE IF EXISTS regress_priv_user5;
DROP ROLE IF EXISTS regress_priv_user6;

SELECT lo_unlink(oid) FROM pg_largeobject_metadata WHERE oid >= 1000 AND oid < 3000 ORDER BY oid;

RESET client_min_messages;

-- test proper begins here

CREATE USER regress_priv_user1;
CREATE USER regress_priv_user2;
CREATE USER regress_priv_user3;
CREATE USER regress_priv_user4;
CREATE USER regress_priv_user5;
CREATE USER regress_priv_user5;	-- duplicate

CREATE GROUP regress_priv_group1;
CREATE GROUP regress_priv_group2 WITH USER regress_priv_user1, regress_priv_user2;

ALTER GROUP regress_priv_group1 ADD USER regress_priv_user4;

ALTER GROUP regress_priv_group2 ADD USER regress_priv_user2;	-- duplicate
ALTER GROUP regress_priv_group2 DROP USER regress_priv_user2;
GRANT regress_priv_group2 TO regress_priv_user4 WITH ADMIN OPTION;

-- test owner privileges

SET SESSION AUTHORIZATION regress_priv_user1;
SELECT session_user, current_user;

CREATE TABLE atest1 ( a int, b text );
SELECT * FROM atest1;
INSERT INTO atest1 VALUES (1, 'one');
DELETE FROM atest1;
UPDATE atest1 SET a = 1 WHERE b = 'blech';
TRUNCATE atest1;
BEGIN;
LOCK atest1 IN ACCESS EXCLUSIVE MODE;
COMMIT;

REVOKE ALL ON atest1 FROM PUBLIC;
SELECT * FROM atest1;

GRANT ALL ON atest1 TO regress_priv_user2;
GRANT SELECT ON atest1 TO regress_priv_user3, regress_priv_user4;
SELECT * FROM atest1;

CREATE TABLE atest2 (col1 varchar(10), col2 boolean);
GRANT SELECT ON atest2 TO regress_priv_user2;
GRANT UPDATE ON atest2 TO regress_priv_user3;
GRANT INSERT ON atest2 TO regress_priv_user4;
GRANT TRUNCATE ON atest2 TO regress_priv_user5;


SET SESSION AUTHORIZATION regress_priv_user2;
SELECT session_user, current_user;

-- try various combinations of queries on atest1 and atest2

SELECT * FROM atest1; -- ok
SELECT * FROM atest2; -- ok
INSERT INTO atest1 VALUES (2, 'two'); -- ok
INSERT INTO atest2 VALUES ('foo', true); -- fail
INSERT INTO atest1 SELECT 1, b FROM atest1; -- ok
UPDATE atest1 SET a = 1 WHERE a = 2; -- ok
UPDATE atest2 SET col2 = NOT col2; -- fail
SELECT * FROM atest1 FOR UPDATE; -- ok
SELECT * FROM atest2 FOR UPDATE; -- fail
DELETE FROM atest2; -- fail
TRUNCATE atest2; -- fail
BEGIN;
LOCK atest2 IN ACCESS EXCLUSIVE MODE; -- fail
COMMIT;
COPY atest2 FROM stdin; -- fail
GRANT ALL ON atest1 TO PUBLIC; -- fail

-- checks in subquery, both ok
SELECT * FROM atest1 WHERE ( b IN ( SELECT col1 FROM atest2 ) );
SELECT * FROM atest2 WHERE ( col1 IN ( SELECT b FROM atest1 ) );


SET SESSION AUTHORIZATION regress_priv_user3;
SELECT session_user, current_user;

SELECT * FROM atest1; -- ok
SELECT * FROM atest2; -- fail
INSERT INTO atest1 VALUES (2, 'two'); -- fail
INSERT INTO atest2 VALUES ('foo', true); -- fail
INSERT INTO atest1 SELECT 1, b FROM atest1; -- fail
UPDATE atest1 SET a = 1 WHERE a = 2; -- fail
UPDATE atest2 SET col2 = NULL; -- ok
UPDATE atest2 SET col2 = NOT col2; -- fails; requires SELECT on atest2
UPDATE atest2 SET col2 = true FROM atest1 WHERE atest1.a = 5; -- ok
SELECT * FROM atest1 FOR UPDATE; -- fail
SELECT * FROM atest2 FOR UPDATE; -- fail
DELETE FROM atest2; -- fail
TRUNCATE atest2; -- fail
BEGIN;
LOCK atest2 IN ACCESS EXCLUSIVE MODE; -- ok
COMMIT;
COPY atest2 FROM stdin; -- fail

-- checks in subquery, both fail
SELECT * FROM atest1 WHERE ( b IN ( SELECT col1 FROM atest2 ) );
SELECT * FROM atest2 WHERE ( col1 IN ( SELECT b FROM atest1 ) );

SET SESSION AUTHORIZATION regress_priv_user4;
COPY atest2 FROM stdin; -- ok
bar	true
\.
SELECT * FROM atest1; -- ok


-- test leaky-function protections in selfuncs

-- regress_priv_user1 will own a table and provide views for it.
SET SESSION AUTHORIZATION regress_priv_user1;

CREATE TABLE atest12 as
  SELECT x AS a, 10001 - x AS b FROM generate_series(1,10000) x;
CREATE INDEX ON atest12 (a);
CREATE INDEX ON atest12 (abs(a));
-- results below depend on having quite accurate stats for atest12, so...
ALTER TABLE atest12 SET (autovacuum_enabled = off);
SET default_statistics_target = 10000;
VACUUM ANALYZE atest12;
RESET default_statistics_target;

CREATE FUNCTION leak(integer,integer) RETURNS boolean
  AS $$begin return $1 < $2; end$$
  LANGUAGE plpgsql immutable;
CREATE OPERATOR <<< (procedure = leak, leftarg = integer, rightarg = integer,
                     restrict = scalarltsel);

-- views with leaky operator
CREATE VIEW atest12v AS
  SELECT * FROM atest12 WHERE b <<< 5;
CREATE VIEW atest12sbv WITH (security_barrier=true) AS
  SELECT * FROM atest12 WHERE b <<< 5;
GRANT SELECT ON atest12v TO PUBLIC;
GRANT SELECT ON atest12sbv TO PUBLIC;

-- This plan should use nestloop, knowing that few rows will be selected.
EXPLAIN (COSTS OFF) SELECT * FROM atest12v x, atest12v y WHERE x.a = y.b;

-- And this one.
EXPLAIN (COSTS OFF) SELECT * FROM atest12 x, atest12 y
  WHERE x.a = y.b and abs(y.a) <<< 5;

-- This should also be a nestloop, but the security barrier forces the inner
-- scan to be materialized
EXPLAIN (COSTS OFF) SELECT * FROM atest12sbv x, atest12sbv y WHERE x.a = y.b;

-- Check if regress_priv_user2 can break security.
SET SESSION AUTHORIZATION regress_priv_user2;

CREATE FUNCTION leak2(integer,integer) RETURNS boolean
  AS $$begin raise notice 'leak % %', $1, $2; return $1 > $2; end$$
  LANGUAGE plpgsql immutable;
CREATE OPERATOR >>> (procedure = leak2, leftarg = integer, rightarg = integer,
                     restrict = scalargtsel);

-- This should not show any "leak" notices before failing.
EXPLAIN (COSTS OFF) SELECT * FROM atest12 WHERE a >>> 0;

-- These plans should continue to use a nestloop, since they execute with the
-- privileges of the view owner.
EXPLAIN (COSTS OFF) SELECT * FROM atest12v x, atest12v y WHERE x.a = y.b;
EXPLAIN (COSTS OFF) SELECT * FROM atest12sbv x, atest12sbv y WHERE x.a = y.b;

-- A non-security barrier view does not guard against information leakage.
EXPLAIN (COSTS OFF) SELECT * FROM atest12v x, atest12v y
  WHERE x.a = y.b and abs(y.a) <<< 5;

-- But a security barrier view isolates the leaky operator.
EXPLAIN (COSTS OFF) SELECT * FROM atest12sbv x, atest12sbv y
  WHERE x.a = y.b and abs(y.a) <<< 5;

-- Now regress_priv_user1 grants sufficient access to regress_priv_user2.
SET SESSION AUTHORIZATION regress_priv_user1;
GRANT SELECT (a, b) ON atest12 TO PUBLIC;
SET SESSION AUTHORIZATION regress_priv_user2;

-- regress_priv_user2 should continue to get a good row estimate.
EXPLAIN (COSTS OFF) SELECT * FROM atest12v x, atest12v y WHERE x.a = y.b;

-- But not for this, due to lack of table-wide permissions needed
-- to make use of the expression index's statistics.
EXPLAIN (COSTS OFF) SELECT * FROM atest12 x, atest12 y
  WHERE x.a = y.b and abs(y.a) <<< 5;

-- clean up (regress_priv_user1's objects are all dropped later)
DROP FUNCTION leak2(integer, integer) CASCADE;


-- groups

SET SESSION AUTHORIZATION regress_priv_user3;
CREATE TABLE atest3 (one int, two int, three int);
GRANT DELETE ON atest3 TO GROUP regress_priv_group2;

SET SESSION AUTHORIZATION regress_priv_user1;

SELECT * FROM atest3; -- fail
DELETE FROM atest3; -- ok


-- views

SET SESSION AUTHORIZATION regress_priv_user3;

CREATE VIEW atestv1 AS SELECT * FROM atest1; -- ok
/* The next *should* fail, but it's not implemented that way yet. */
CREATE VIEW atestv2 AS SELECT * FROM atest2;
CREATE VIEW atestv3 AS SELECT * FROM atest3; -- ok
/* Empty view is a corner case that failed in 9.2. */
CREATE VIEW atestv0 AS SELECT 0 as x WHERE false; -- ok

SELECT * FROM atestv1; -- ok
SELECT * FROM atestv2; -- fail
GRANT SELECT ON atestv1, atestv3 TO regress_priv_user4;
GRANT SELECT ON atestv2 TO regress_priv_user2;

SET SESSION AUTHORIZATION regress_priv_user4;

SELECT * FROM atestv1; -- ok
SELECT * FROM atestv2; -- fail
SELECT * FROM atestv3; -- ok
SELECT * FROM atestv0; -- fail

-- Appendrels excluded by constraints failed to check permissions in 8.4-9.2.
select * from
  ((select a.q1 as x from int8_tbl a offset 0)
   union all
   (select b.q2 as x from int8_tbl b offset 0)) ss
where false;

set constraint_exclusion = on;
select * from
  ((select a.q1 as x, random() from int8_tbl a where q1 > 0)
   union all
   (select b.q2 as x, random() from int8_tbl b where q2 > 0)) ss
where x < 0;
reset constraint_exclusion;

CREATE VIEW atestv4 AS SELECT * FROM atestv3; -- nested view
SELECT * FROM atestv4; -- ok
GRANT SELECT ON atestv4 TO regress_priv_user2;

SET SESSION AUTHORIZATION regress_priv_user2;

-- Two complex cases:

SELECT * FROM atestv3; -- fail
SELECT * FROM atestv4; -- ok (even though regress_priv_user2 cannot access underlying atestv3)

SELECT * FROM atest2; -- ok
SELECT * FROM atestv2; -- fail (even though regress_priv_user2 can access underlying atest2)

-- Test column level permissions

SET SESSION AUTHORIZATION regress_priv_user1;
CREATE TABLE atest5 (one int, two int unique, three int, four int unique);
CREATE TABLE atest6 (one int, two int, blue int);
GRANT SELECT (one), INSERT (two), UPDATE (three) ON atest5 TO regress_priv_user4;
GRANT ALL (one) ON atest5 TO regress_priv_user3;

INSERT INTO atest5 VALUES (1,2,3);

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT * FROM atest5; -- fail
SELECT one FROM atest5; -- ok
COPY atest5 (one) TO stdout; -- ok
SELECT two FROM atest5; -- fail
COPY atest5 (two) TO stdout; -- fail
SELECT atest5 FROM atest5; -- fail
COPY atest5 (one,two) TO stdout; -- fail
SELECT 1 FROM atest5; -- ok
SELECT 1 FROM atest5 a JOIN atest5 b USING (one); -- ok
SELECT 1 FROM atest5 a JOIN atest5 b USING (two); -- fail
SELECT 1 FROM atest5 a NATURAL JOIN atest5 b; -- fail
SELECT (j.*) IS NULL FROM (atest5 a JOIN atest5 b USING (one)) j; -- fail
SELECT 1 FROM atest5 WHERE two = 2; -- fail
SELECT * FROM atest1, atest5; -- fail
SELECT atest1.* FROM atest1, atest5; -- ok
SELECT atest1.*,atest5.one FROM atest1, atest5; -- ok
SELECT atest1.*,atest5.one FROM atest1 JOIN atest5 ON (atest1.a = atest5.two); -- fail
SELECT atest1.*,atest5.one FROM atest1 JOIN atest5 ON (atest1.a = atest5.one); -- ok
SELECT one, two FROM atest5; -- fail

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT SELECT (one,two) ON atest6 TO regress_priv_user4;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT one, two FROM atest5 NATURAL JOIN atest6; -- fail still

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT SELECT (two) ON atest5 TO regress_priv_user4;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT one, two FROM atest5 NATURAL JOIN atest6; -- ok now

-- test column-level privileges for INSERT and UPDATE
INSERT INTO atest5 (two) VALUES (3); -- ok
COPY atest5 FROM stdin; -- fail
COPY atest5 (two) FROM stdin; -- ok
1
\.
INSERT INTO atest5 (three) VALUES (4); -- fail
INSERT INTO atest5 VALUES (5,5,5); -- fail
UPDATE atest5 SET three = 10; -- ok
UPDATE atest5 SET one = 8; -- fail
UPDATE atest5 SET three = 5, one = 2; -- fail
-- Check that column level privs are enforced in RETURNING
-- Ok.
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set three = 10;
-- Error. No SELECT on column three.
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set three = 10 RETURNING atest5.three;
-- Ok.  May SELECT on column "one":
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set three = 10 RETURNING atest5.one;
-- Check that column level privileges are enforced for EXCLUDED
-- Ok. we may select one
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set three = EXCLUDED.one;
-- Error. No select rights on three
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set three = EXCLUDED.three;
INSERT INTO atest5(two) VALUES (6) ON CONFLICT (two) DO UPDATE set one = 8; -- fails (due to UPDATE)
INSERT INTO atest5(three) VALUES (4) ON CONFLICT (two) DO UPDATE set three = 10; -- fails (due to INSERT)

-- Check that the columns in the inference require select privileges
INSERT INTO atest5(four) VALUES (4); -- fail

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT INSERT (four) ON atest5 TO regress_priv_user4;
SET SESSION AUTHORIZATION regress_priv_user4;

INSERT INTO atest5(four) VALUES (4) ON CONFLICT (four) DO UPDATE set three = 3; -- fails (due to SELECT)
INSERT INTO atest5(four) VALUES (4) ON CONFLICT ON CONSTRAINT atest5_four_key DO UPDATE set three = 3; -- fails (due to SELECT)
INSERT INTO atest5(four) VALUES (4); -- ok

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT SELECT (four) ON atest5 TO regress_priv_user4;
SET SESSION AUTHORIZATION regress_priv_user4;

INSERT INTO atest5(four) VALUES (4) ON CONFLICT (four) DO UPDATE set three = 3; -- ok
INSERT INTO atest5(four) VALUES (4) ON CONFLICT ON CONSTRAINT atest5_four_key DO UPDATE set three = 3; -- ok

SET SESSION AUTHORIZATION regress_priv_user1;
REVOKE ALL (one) ON atest5 FROM regress_priv_user4;
GRANT SELECT (one,two,blue) ON atest6 TO regress_priv_user4;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT one FROM atest5; -- fail
UPDATE atest5 SET one = 1; -- fail
SELECT atest6 FROM atest6; -- ok
COPY atest6 TO stdout; -- ok

-- check error reporting with column privs
SET SESSION AUTHORIZATION regress_priv_user1;
CREATE TABLE t1 (c1 int, c2 int, c3 int check (c3 < 5), primary key (c1, c2));
GRANT SELECT (c1) ON t1 TO regress_priv_user2;
GRANT INSERT (c1, c2, c3) ON t1 TO regress_priv_user2;
GRANT UPDATE (c1, c2, c3) ON t1 TO regress_priv_user2;

-- seed data
INSERT INTO t1 VALUES (1, 1, 1);
INSERT INTO t1 VALUES (1, 2, 1);
INSERT INTO t1 VALUES (2, 1, 2);
INSERT INTO t1 VALUES (2, 2, 2);
INSERT INTO t1 VALUES (3, 1, 3);

SET SESSION AUTHORIZATION regress_priv_user2;
INSERT INTO t1 (c1, c2) VALUES (1, 1); -- fail, but row not shown
UPDATE t1 SET c2 = 1; -- fail, but row not shown
INSERT INTO t1 (c1, c2) VALUES (null, null); -- fail, but see columns being inserted
INSERT INTO t1 (c3) VALUES (null); -- fail, but see columns being inserted or have SELECT
INSERT INTO t1 (c1) VALUES (5); -- fail, but see columns being inserted or have SELECT
UPDATE t1 SET c3 = 10; -- fail, but see columns with SELECT rights, or being modified

SET SESSION AUTHORIZATION regress_priv_user1;
DROP TABLE t1;

-- test column-level privileges when involved with DELETE
SET SESSION AUTHORIZATION regress_priv_user1;
ALTER TABLE atest6 ADD COLUMN three integer;
GRANT DELETE ON atest5 TO regress_priv_user3;
GRANT SELECT (two) ON atest5 TO regress_priv_user3;
REVOKE ALL (one) ON atest5 FROM regress_priv_user3;
GRANT SELECT (one) ON atest5 TO regress_priv_user4;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT atest6 FROM atest6; -- fail
SELECT one FROM atest5 NATURAL JOIN atest6; -- fail

SET SESSION AUTHORIZATION regress_priv_user1;
ALTER TABLE atest6 DROP COLUMN three;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT atest6 FROM atest6; -- ok
SELECT one FROM atest5 NATURAL JOIN atest6; -- ok

SET SESSION AUTHORIZATION regress_priv_user1;
ALTER TABLE atest6 DROP COLUMN two;
REVOKE SELECT (one,blue) ON atest6 FROM regress_priv_user4;

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT * FROM atest6; -- fail
SELECT 1 FROM atest6; -- fail

SET SESSION AUTHORIZATION regress_priv_user3;
DELETE FROM atest5 WHERE one = 1; -- fail
DELETE FROM atest5 WHERE two = 2; -- ok

-- check inheritance cases
SET SESSION AUTHORIZATION regress_priv_user1;
CREATE TABLE atestp1 (f1 int, f2 int);
CREATE TABLE atestp2 (fx int, fy int);
CREATE TABLE atestc (fz int) INHERITS (atestp1, atestp2);
GRANT SELECT(fx,fy,tableoid) ON atestp2 TO regress_priv_user2;
GRANT SELECT(fx) ON atestc TO regress_priv_user2;

SET SESSION AUTHORIZATION regress_priv_user2;
SELECT fx FROM atestp2; -- ok
SELECT fy FROM atestp2; -- ok
SELECT atestp2 FROM atestp2; -- ok
SELECT tableoid FROM atestp2; -- ok
SELECT fy FROM atestc; -- fail

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT SELECT(fy,tableoid) ON atestc TO regress_priv_user2;

SET SESSION AUTHORIZATION regress_priv_user2;
SELECT fx FROM atestp2; -- still ok
SELECT fy FROM atestp2; -- ok
SELECT atestp2 FROM atestp2; -- ok
SELECT tableoid FROM atestp2; -- ok

-- child's permissions do not apply when operating on parent
SET SESSION AUTHORIZATION regress_priv_user1;
REVOKE ALL ON atestc FROM regress_priv_user2;
GRANT ALL ON atestp1 TO regress_priv_user2;
SET SESSION AUTHORIZATION regress_priv_user2;
SELECT f2 FROM atestp1; -- ok
SELECT f2 FROM atestc; -- fail
DELETE FROM atestp1; -- ok
DELETE FROM atestc; -- fail
UPDATE atestp1 SET f1 = 1; -- ok
UPDATE atestc SET f1 = 1; -- fail
TRUNCATE atestp1; -- ok
TRUNCATE atestc; -- fail
BEGIN;
LOCK atestp1;
END;
BEGIN;
LOCK atestc;
END;

-- privileges on functions, languages

-- switch to superuser
\c -

REVOKE ALL PRIVILEGES ON LANGUAGE sql FROM PUBLIC;
GRANT USAGE ON LANGUAGE sql TO regress_priv_user1; -- ok
GRANT USAGE ON LANGUAGE c TO PUBLIC; -- fail

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT USAGE ON LANGUAGE sql TO regress_priv_user2; -- fail
CREATE FUNCTION priv_testfunc1(int) RETURNS int AS 'select 2 * $1;' LANGUAGE sql;
CREATE FUNCTION priv_testfunc2(int) RETURNS int AS 'select 3 * $1;' LANGUAGE sql;
CREATE AGGREGATE priv_testagg1(int) (sfunc = int4pl, stype = int4);
CREATE PROCEDURE priv_testproc1(int) AS 'select $1;' LANGUAGE sql;

REVOKE ALL ON FUNCTION priv_testfunc1(int), priv_testfunc2(int), priv_testagg1(int) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION priv_testfunc1(int), priv_testfunc2(int), priv_testagg1(int) TO regress_priv_user2;
REVOKE ALL ON FUNCTION priv_testproc1(int) FROM PUBLIC; -- fail, not a function
REVOKE ALL ON PROCEDURE priv_testproc1(int) FROM PUBLIC;
GRANT EXECUTE ON PROCEDURE priv_testproc1(int) TO regress_priv_user2;
GRANT USAGE ON FUNCTION priv_testfunc1(int) TO regress_priv_user3; -- semantic error
GRANT USAGE ON FUNCTION priv_testagg1(int) TO regress_priv_user3; -- semantic error
GRANT USAGE ON PROCEDURE priv_testproc1(int) TO regress_priv_user3; -- semantic error
GRANT ALL PRIVILEGES ON FUNCTION priv_testfunc1(int) TO regress_priv_user4;
GRANT ALL PRIVILEGES ON FUNCTION priv_testfunc_nosuch(int) TO regress_priv_user4;
GRANT ALL PRIVILEGES ON FUNCTION priv_testagg1(int) TO regress_priv_user4;
GRANT ALL PRIVILEGES ON PROCEDURE priv_testproc1(int) TO regress_priv_user4;

CREATE FUNCTION priv_testfunc4(boolean) RETURNS text
  AS 'select col1 from atest2 where col2 = $1;'
  LANGUAGE sql SECURITY DEFINER;
GRANT EXECUTE ON FUNCTION priv_testfunc4(boolean) TO regress_priv_user3;

SET SESSION AUTHORIZATION regress_priv_user2;
SELECT priv_testfunc1(5), priv_testfunc2(5); -- ok
CREATE FUNCTION priv_testfunc3(int) RETURNS int AS 'select 2 * $1;' LANGUAGE sql; -- fail
SELECT priv_testagg1(x) FROM (VALUES (1), (2), (3)) _(x); -- ok
CALL priv_testproc1(6); -- ok

SET SESSION AUTHORIZATION regress_priv_user3;
SELECT priv_testfunc1(5); -- fail
SELECT priv_testagg1(x) FROM (VALUES (1), (2), (3)) _(x); -- fail
CALL priv_testproc1(6); -- fail
SELECT col1 FROM atest2 WHERE col2 = true; -- fail
SELECT priv_testfunc4(true); -- ok

SET SESSION AUTHORIZATION regress_priv_user4;
SELECT priv_testfunc1(5); -- ok
SELECT priv_testagg1(x) FROM (VALUES (1), (2), (3)) _(x); -- ok
CALL priv_testproc1(6); -- ok

DROP FUNCTION priv_testfunc1(int); -- fail
DROP AGGREGATE priv_testagg1(int); -- fail
DROP PROCEDURE priv_testproc1(int); -- fail

\c -

DROP FUNCTION priv_testfunc1(int); -- ok
-- restore to sanity
GRANT ALL PRIVILEGES ON LANGUAGE sql TO PUBLIC;

-- verify privilege checks on array-element coercions
BEGIN;
SELECT '{1}'::int4[]::int8[];
REVOKE ALL ON FUNCTION int8(integer) FROM PUBLIC;
SELECT '{1}'::int4[]::int8[]; --superuser, succeed
SET SESSION AUTHORIZATION regress_priv_user4;
SELECT '{1}'::int4[]::int8[]; --other user, fail
ROLLBACK;

-- privileges on types

-- switch to superuser
\c -

CREATE TYPE priv_testtype1 AS (a int, b text);
REVOKE USAGE ON TYPE priv_testtype1 FROM PUBLIC;
GRANT USAGE ON TYPE priv_testtype1 TO regress_priv_user2;
GRANT USAGE ON TYPE _priv_testtype1 TO regress_priv_user2; -- fail
GRANT USAGE ON DOMAIN priv_testtype1 TO regress_priv_user2; -- fail

CREATE DOMAIN priv_testdomain1 AS int;
REVOKE USAGE on DOMAIN priv_testdomain1 FROM PUBLIC;
GRANT USAGE ON DOMAIN priv_testdomain1 TO regress_priv_user2;
GRANT USAGE ON TYPE priv_testdomain1 TO regress_priv_user2; -- ok

SET SESSION AUTHORIZATION regress_priv_user1;

-- commands that should fail

CREATE AGGREGATE priv_testagg1a(priv_testdomain1) (sfunc = int4_sum, stype = bigint);

CREATE DOMAIN priv_testdomain2a AS priv_testdomain1;

CREATE DOMAIN priv_testdomain3a AS int;
CREATE FUNCTION castfunc(int) RETURNS priv_testdomain3a AS $$ SELECT $1::priv_testdomain3a $$ LANGUAGE SQL;
CREATE CAST (priv_testdomain1 AS priv_testdomain3a) WITH FUNCTION castfunc(int);
DROP FUNCTION castfunc(int) CASCADE;
DROP DOMAIN priv_testdomain3a;

CREATE FUNCTION priv_testfunc5a(a priv_testdomain1) RETURNS int LANGUAGE SQL AS $$ SELECT $1 $$;
CREATE FUNCTION priv_testfunc6a(b int) RETURNS priv_testdomain1 LANGUAGE SQL AS $$ SELECT $1::priv_testdomain1 $$;

CREATE OPERATOR !+! (PROCEDURE = int4pl, LEFTARG = priv_testdomain1, RIGHTARG = priv_testdomain1);

CREATE TABLE test5a (a int, b priv_testdomain1);
CREATE TABLE test6a OF priv_testtype1;
CREATE TABLE test10a (a int[], b priv_testtype1[]);

CREATE TABLE test9a (a int, b int);
ALTER TABLE test9a ADD COLUMN c priv_testdomain1;
ALTER TABLE test9a ALTER COLUMN b TYPE priv_testdomain1;

CREATE TYPE test7a AS (a int, b priv_testdomain1);

CREATE TYPE test8a AS (a int, b int);
ALTER TYPE test8a ADD ATTRIBUTE c priv_testdomain1;
ALTER TYPE test8a ALTER ATTRIBUTE b TYPE priv_testdomain1;

CREATE TABLE test11a AS (SELECT 1::priv_testdomain1 AS a);

REVOKE ALL ON TYPE priv_testtype1 FROM PUBLIC;

SET SESSION AUTHORIZATION regress_priv_user2;

-- commands that should succeed

CREATE AGGREGATE priv_testagg1b(priv_testdomain1) (sfunc = int4_sum, stype = bigint);

CREATE DOMAIN priv_testdomain2b AS priv_testdomain1;

CREATE DOMAIN priv_testdomain3b AS int;
CREATE FUNCTION castfunc(int) RETURNS priv_testdomain3b AS $$ SELECT $1::priv_testdomain3b $$ LANGUAGE SQL;
CREATE CAST (priv_testdomain1 AS priv_testdomain3b) WITH FUNCTION castfunc(int);

CREATE FUNCTION priv_testfunc5b(a priv_testdomain1) RETURNS int LANGUAGE SQL AS $$ SELECT $1 $$;
CREATE FUNCTION priv_testfunc6b(b int) RETURNS priv_testdomain1 LANGUAGE SQL AS $$ SELECT $1::priv_testdomain1 $$;

CREATE OPERATOR !! (PROCEDURE = priv_testfunc5b, RIGHTARG = priv_testdomain1);

CREATE TABLE test5b (a int, b priv_testdomain1);
CREATE TABLE test6b OF priv_testtype1;
CREATE TABLE test10b (a int[], b priv_testtype1[]);

CREATE TABLE test9b (a int, b int);
ALTER TABLE test9b ADD COLUMN c priv_testdomain1;
ALTER TABLE test9b ALTER COLUMN b TYPE priv_testdomain1;

CREATE TYPE test7b AS (a int, b priv_testdomain1);

CREATE TYPE test8b AS (a int, b int);
ALTER TYPE test8b ADD ATTRIBUTE c priv_testdomain1;
ALTER TYPE test8b ALTER ATTRIBUTE b TYPE priv_testdomain1;

CREATE TABLE test11b AS (SELECT 1::priv_testdomain1 AS a);

REVOKE ALL ON TYPE priv_testtype1 FROM PUBLIC;

\c -
DROP AGGREGATE priv_testagg1b(priv_testdomain1);
DROP DOMAIN priv_testdomain2b;
DROP OPERATOR !! (NONE, priv_testdomain1);
DROP FUNCTION priv_testfunc5b(a priv_testdomain1);
DROP FUNCTION priv_testfunc6b(b int);
DROP TABLE test5b;
DROP TABLE test6b;
DROP TABLE test9b;
DROP TABLE test10b;
DROP TYPE test7b;
DROP TYPE test8b;
DROP CAST (priv_testdomain1 AS priv_testdomain3b);
DROP FUNCTION castfunc(int) CASCADE;
DROP DOMAIN priv_testdomain3b;
DROP TABLE test11b;

DROP TYPE priv_testtype1; -- ok
DROP DOMAIN priv_testdomain1; -- ok


-- truncate
SET SESSION AUTHORIZATION regress_priv_user5;
TRUNCATE atest2; -- ok
TRUNCATE atest3; -- fail

-- has_table_privilege function

-- bad-input checks
select has_table_privilege(NULL,'pg_authid','select');
select has_table_privilege('pg_shad','select');
select has_table_privilege('nosuchuser','pg_authid','select');
select has_table_privilege('pg_authid','sel');
select has_table_privilege(-999999,'pg_authid','update');
select has_table_privilege(1,'select');

-- superuser
\c -

select has_table_privilege(current_user,'pg_authid','select');
select has_table_privilege(current_user,'pg_authid','insert');

select has_table_privilege(t2.oid,'pg_authid','update')
from (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,'pg_authid','delete')
from (select oid from pg_roles where rolname = current_user) as t2;

-- 'rule' privilege no longer exists, but for backwards compatibility
-- has_table_privilege still recognizes the keyword and says FALSE
select has_table_privilege(current_user,t1.oid,'rule')
from (select oid from pg_class where relname = 'pg_authid') as t1;
select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'pg_authid') as t1;

select has_table_privilege(t2.oid,t1.oid,'select')
from (select oid from pg_class where relname = 'pg_authid') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,t1.oid,'insert')
from (select oid from pg_class where relname = 'pg_authid') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;

select has_table_privilege('pg_authid','update');
select has_table_privilege('pg_authid','delete');
select has_table_privilege('pg_authid','truncate');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'pg_authid') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'pg_authid') as t1;

-- non-superuser
SET SESSION AUTHORIZATION regress_priv_user3;

select has_table_privilege(current_user,'pg_class','select');
select has_table_privilege(current_user,'pg_class','insert');

select has_table_privilege(t2.oid,'pg_class','update')
from (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,'pg_class','delete')
from (select oid from pg_roles where rolname = current_user) as t2;

select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'pg_class') as t1;

select has_table_privilege(t2.oid,t1.oid,'select')
from (select oid from pg_class where relname = 'pg_class') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,t1.oid,'insert')
from (select oid from pg_class where relname = 'pg_class') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;

select has_table_privilege('pg_class','update');
select has_table_privilege('pg_class','delete');
select has_table_privilege('pg_class','truncate');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'pg_class') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'pg_class') as t1;

select has_table_privilege(current_user,'atest1','select');
select has_table_privilege(current_user,'atest1','insert');

select has_table_privilege(t2.oid,'atest1','update')
from (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,'atest1','delete')
from (select oid from pg_roles where rolname = current_user) as t2;

select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'atest1') as t1;

select has_table_privilege(t2.oid,t1.oid,'select')
from (select oid from pg_class where relname = 'atest1') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;
select has_table_privilege(t2.oid,t1.oid,'insert')
from (select oid from pg_class where relname = 'atest1') as t1,
  (select oid from pg_roles where rolname = current_user) as t2;

select has_table_privilege('atest1','update');
select has_table_privilege('atest1','delete');
select has_table_privilege('atest1','truncate');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'atest1') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'atest1') as t1;

-- has_column_privilege function

-- bad-input checks (as non-super-user)
select has_column_privilege('pg_authid',NULL,'select');
select has_column_privilege('pg_authid','nosuchcol','select');
select has_column_privilege(9999,'nosuchcol','select');
select has_column_privilege(9999,99::int2,'select');
select has_column_privilege('pg_authid',99::int2,'select');
select has_column_privilege(9999,99::int2,'select');

create temp table mytable(f1 int, f2 int, f3 int);
alter table mytable drop column f2;
select has_column_privilege('mytable','f2','select');
select has_column_privilege('mytable','........pg.dropped.2........','select');
select has_column_privilege('mytable',2::int2,'select');
revoke select on table mytable from regress_priv_user3;
select has_column_privilege('mytable',2::int2,'select');
drop table mytable;

-- Grant options

SET SESSION AUTHORIZATION regress_priv_user1;

CREATE TABLE atest4 (a int);

GRANT SELECT ON atest4 TO regress_priv_user2 WITH GRANT OPTION;
GRANT UPDATE ON atest4 TO regress_priv_user2;
GRANT SELECT ON atest4 TO GROUP regress_priv_group1 WITH GRANT OPTION;

SET SESSION AUTHORIZATION regress_priv_user2;

GRANT SELECT ON atest4 TO regress_priv_user3;
GRANT UPDATE ON atest4 TO regress_priv_user3; -- fail

SET SESSION AUTHORIZATION regress_priv_user1;

REVOKE SELECT ON atest4 FROM regress_priv_user3; -- does nothing
SELECT has_table_privilege('regress_priv_user3', 'atest4', 'SELECT'); -- true
REVOKE SELECT ON atest4 FROM regress_priv_user2; -- fail
REVOKE GRANT OPTION FOR SELECT ON atest4 FROM regress_priv_user2 CASCADE; -- ok
SELECT has_table_privilege('regress_priv_user2', 'atest4', 'SELECT'); -- true
SELECT has_table_privilege('regress_priv_user3', 'atest4', 'SELECT'); -- false

SELECT has_table_privilege('regress_priv_user1', 'atest4', 'SELECT WITH GRANT OPTION'); -- true


-- Admin options

SET SESSION AUTHORIZATION regress_priv_user4;
CREATE FUNCTION dogrant_ok() RETURNS void LANGUAGE sql SECURITY DEFINER AS
	'GRANT regress_priv_group2 TO regress_priv_user5';
GRANT regress_priv_group2 TO regress_priv_user5; -- ok: had ADMIN OPTION
SET ROLE regress_priv_group2;
GRANT regress_priv_group2 TO regress_priv_user5; -- fails: SET ROLE suspended privilege

SET SESSION AUTHORIZATION regress_priv_user1;
GRANT regress_priv_group2 TO regress_priv_user5; -- fails: no ADMIN OPTION
SELECT dogrant_ok();			-- ok: SECURITY DEFINER conveys ADMIN
SET ROLE regress_priv_group2;
GRANT regress_priv_group2 TO regress_priv_user5; -- fails: SET ROLE did not help

SET SESSION AUTHORIZATION regress_priv_group2;
GRANT regress_priv_group2 TO regress_priv_user5; -- ok: a role can self-admin
CREATE FUNCTION dogrant_fails() RETURNS void LANGUAGE sql SECURITY DEFINER AS
	'GRANT regress_priv_group2 TO regress_priv_user5';
SELECT dogrant_fails();			-- fails: no self-admin in SECURITY DEFINER
DROP FUNCTION dogrant_fails();

SET SESSION AUTHORIZATION regress_priv_user4;
DROP FUNCTION dogrant_ok();
REVOKE regress_priv_group2 FROM regress_priv_user5;


-- has_sequence_privilege tests
\c -

CREATE SEQUENCE x_seq;

GRANT USAGE on x_seq to regress_priv_user2;

SELECT has_sequence_privilege('regress_priv_user1', 'atest1', 'SELECT');
SELECT has_sequence_privilege('regress_priv_user1', 'x_seq', 'INSERT');
SELECT has_sequence_privilege('regress_priv_user1', 'x_seq', 'SELECT');

SET SESSION AUTHORIZATION regress_priv_user2;

SELECT has_sequence_privilege('x_seq', 'USAGE');

-- largeobject privilege tests
\c -
SET SESSION AUTHORIZATION regress_priv_user1;

SELECT lo_create(1001);
SELECT lo_create(1002);
SELECT lo_create(1003);
SELECT lo_create(1004);
SELECT lo_create(1005);

GRANT ALL ON LARGE OBJECT 1001 TO PUBLIC;
GRANT SELECT ON LARGE OBJECT 1003 TO regress_priv_user2;
GRANT SELECT,UPDATE ON LARGE OBJECT 1004 TO regress_priv_user2;
GRANT ALL ON LARGE OBJECT 1005 TO regress_priv_user2;
GRANT SELECT ON LARGE OBJECT 1005 TO regress_priv_user2 WITH GRANT OPTION;

GRANT SELECT, INSERT ON LARGE OBJECT 1001 TO PUBLIC;	-- to be failed
GRANT SELECT, UPDATE ON LARGE OBJECT 1001 TO nosuchuser;	-- to be failed
GRANT SELECT, UPDATE ON LARGE OBJECT  999 TO PUBLIC;	-- to be failed

\c -
SET SESSION AUTHORIZATION regress_priv_user2;

SELECT lo_create(2001);
SELECT lo_create(2002);

SELECT loread(lo_open(1001, x'20000'::int), 32);	-- allowed, for now
SELECT lowrite(lo_open(1001, x'40000'::int), 'abcd');	-- fail, wrong mode

SELECT loread(lo_open(1001, x'40000'::int), 32);
SELECT loread(lo_open(1002, x'40000'::int), 32);	-- to be denied
SELECT loread(lo_open(1003, x'40000'::int), 32);
SELECT loread(lo_open(1004, x'40000'::int), 32);

SELECT lowrite(lo_open(1001, x'20000'::int), 'abcd');
SELECT lowrite(lo_open(1002, x'20000'::int), 'abcd');	-- to be denied
SELECT lowrite(lo_open(1003, x'20000'::int), 'abcd');	-- to be denied
SELECT lowrite(lo_open(1004, x'20000'::int), 'abcd');

GRANT SELECT ON LARGE OBJECT 1005 TO regress_priv_user3;
GRANT UPDATE ON LARGE OBJECT 1006 TO regress_priv_user3;	-- to be denied
REVOKE ALL ON LARGE OBJECT 2001, 2002 FROM PUBLIC;
GRANT ALL ON LARGE OBJECT 2001 TO regress_priv_user3;

SELECT lo_unlink(1001);		-- to be denied
SELECT lo_unlink(2002);

\c -
-- confirm ACL setting
SELECT oid, pg_get_userbyid(lomowner) ownername, lomacl FROM pg_largeobject_metadata WHERE oid >= 1000 AND oid < 3000 ORDER BY oid;

SET SESSION AUTHORIZATION regress_priv_user3;

SELECT loread(lo_open(1001, x'40000'::int), 32);
SELECT loread(lo_open(1003, x'40000'::int), 32);	-- to be denied
SELECT loread(lo_open(1005, x'40000'::int), 32);

SELECT lo_truncate(lo_open(1005, x'20000'::int), 10);	-- to be denied
SELECT lo_truncate(lo_open(2001, x'20000'::int), 10);

-- compatibility mode in largeobject permission
\c -
SET lo_compat_privileges = false;	-- default setting
SET SESSION AUTHORIZATION regress_priv_user4;

SELECT loread(lo_open(1002, x'40000'::int), 32);	-- to be denied
SELECT lowrite(lo_open(1002, x'20000'::int), 'abcd');	-- to be denied
SELECT lo_truncate(lo_open(1002, x'20000'::int), 10);	-- to be denied
SELECT lo_put(1002, 1, 'abcd');				-- to be denied
SELECT lo_unlink(1002);					-- to be denied
SELECT lo_export(1001, '/dev/null');			-- to be denied
SELECT lo_import('/dev/null');				-- to be denied
SELECT lo_import('/dev/null', 2003);			-- to be denied

\c -
SET lo_compat_privileges = true;	-- compatibility mode
SET SESSION AUTHORIZATION regress_priv_user4;

SELECT loread(lo_open(1002, x'40000'::int), 32);
SELECT lowrite(lo_open(1002, x'20000'::int), 'abcd');
SELECT lo_truncate(lo_open(1002, x'20000'::int), 10);
SELECT lo_unlink(1002);
SELECT lo_export(1001, '/dev/null');			-- to be denied

-- don't allow unpriv users to access pg_largeobject contents
\c -
SELECT * FROM pg_largeobject LIMIT 0;

SET SESSION AUTHORIZATION regress_priv_user1;
SELECT * FROM pg_largeobject LIMIT 0;			-- to be denied

-- test default ACLs
\c -

CREATE SCHEMA testns;
GRANT ALL ON SCHEMA testns TO regress_priv_user1;

CREATE TABLE testns.acltest1 (x int);
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'SELECT'); -- no
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'INSERT'); -- no

ALTER DEFAULT PRIVILEGES IN SCHEMA testns GRANT SELECT ON TABLES TO public;

SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'SELECT'); -- no
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'INSERT'); -- no

DROP TABLE testns.acltest1;
CREATE TABLE testns.acltest1 (x int);

SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'SELECT'); -- yes
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'INSERT'); -- no

ALTER DEFAULT PRIVILEGES IN SCHEMA testns GRANT INSERT ON TABLES TO regress_priv_user1;

DROP TABLE testns.acltest1;
CREATE TABLE testns.acltest1 (x int);

SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'SELECT'); -- yes
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'INSERT'); -- yes

ALTER DEFAULT PRIVILEGES IN SCHEMA testns REVOKE INSERT ON TABLES FROM regress_priv_user1;

DROP TABLE testns.acltest1;
CREATE TABLE testns.acltest1 (x int);

SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'SELECT'); -- yes
SELECT has_table_privilege('regress_priv_user1', 'testns.acltest1', 'INSERT'); -- no

ALTER DEFAULT PRIVILEGES FOR ROLE regress_priv_user1 REVOKE EXECUTE ON FUNCTIONS FROM public;

ALTER DEFAULT PRIVILEGES IN SCHEMA testns GRANT USAGE ON SCHEMAS TO regress_priv_user2; -- error

--
-- Testing blanket default grants is very hazardous since it might change
-- the privileges attached to objects created by concurrent regression tests.
-- To avoid that, be sure to revoke the privileges again before committing.
--
BEGIN;

ALTER DEFAULT PRIVILEGES GRANT USAGE ON SCHEMAS TO regress_priv_user2;

CREATE SCHEMA testns2;

SELECT has_schema_privilege('regress_priv_user2', 'testns2', 'USAGE'); -- yes
SELECT has_schema_privilege('regress_priv_user2', 'testns2', 'CREATE'); -- no

ALTER DEFAULT PRIVILEGES REVOKE USAGE ON SCHEMAS FROM regress_priv_user2;

CREATE SCHEMA testns3;

SELECT has_schema_privilege('regress_priv_user2', 'testns3', 'USAGE'); -- no
SELECT has_schema_privilege('regress_priv_user2', 'testns3', 'CREATE'); -- no

ALTER DEFAULT PRIVILEGES GRANT ALL ON SCHEMAS TO regress_priv_user2;

CREATE SCHEMA testns4;

SELECT has_schema_privilege('regress_priv_user2', 'testns4', 'USAGE'); -- yes
SELECT has_schema_privilege('regress_priv_user2', 'testns4', 'CREATE'); -- yes

ALTER DEFAULT PRIVILEGES REVOKE ALL ON SCHEMAS FROM regress_priv_user2;

COMMIT;

CREATE SCHEMA testns5;

SELECT has_schema_privilege('regress_priv_user2', 'testns5', 'USAGE'); -- no
SELECT has_schema_privilege('regress_priv_user2', 'testns5', 'CREATE'); -- no

SET ROLE regress_priv_user1;

CREATE FUNCTION testns.foo() RETURNS int AS 'select 1' LANGUAGE sql;
CREATE AGGREGATE testns.agg1(int) (sfunc = int4pl, stype = int4);
CREATE PROCEDURE testns.bar() AS 'select 1' LANGUAGE sql;

SELECT has_function_privilege('regress_priv_user2', 'testns.foo()', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_priv_user2', 'testns.agg1(int)', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_priv_user2', 'testns.bar()', 'EXECUTE'); -- no

ALTER DEFAULT PRIVILEGES IN SCHEMA testns GRANT EXECUTE ON ROUTINES to public;

DROP FUNCTION testns.foo();
CREATE FUNCTION testns.foo() RETURNS int AS 'select 1' LANGUAGE sql;
DROP AGGREGATE testns.agg1(int);
CREATE AGGREGATE testns.agg1(int) (sfunc = int4pl, stype = int4);
DROP PROCEDURE testns.bar();
CREATE PROCEDURE testns.bar() AS 'select 1' LANGUAGE sql;

SELECT has_function_privilege('regress_priv_user2', 'testns.foo()', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_priv_user2', 'testns.agg1(int)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_priv_user2', 'testns.bar()', 'EXECUTE'); -- yes (counts as function here)

DROP FUNCTION testns.foo();
DROP AGGREGATE testns.agg1(int);
DROP PROCEDURE testns.bar();

ALTER DEFAULT PRIVILEGES FOR ROLE regress_priv_user1 REVOKE USAGE ON TYPES FROM public;

CREATE DOMAIN testns.priv_testdomain1 AS int;

SELECT has_type_privilege('regress_priv_user2', 'testns.priv_testdomain1', 'USAGE'); -- no

ALTER DEFAULT PRIVILEGES IN SCHEMA testns GRANT USAGE ON TYPES to public;

DROP DOMAIN testns.priv_testdomain1;
CREATE DOMAIN testns.priv_testdomain1 AS int;

SELECT has_type_privilege('regress_priv_user2', 'testns.priv_testdomain1', 'USAGE'); -- yes

DROP DOMAIN testns.priv_testdomain1;

RESET ROLE;

SELECT count(*)
  FROM pg_default_acl d LEFT JOIN pg_namespace n ON defaclnamespace = n.oid
  WHERE nspname = 'testns';

DROP SCHEMA testns CASCADE;
DROP SCHEMA testns2 CASCADE;
DROP SCHEMA testns3 CASCADE;
DROP SCHEMA testns4 CASCADE;
DROP SCHEMA testns5 CASCADE;

SELECT d.*     -- check that entries went away
  FROM pg_default_acl d LEFT JOIN pg_namespace n ON defaclnamespace = n.oid
  WHERE nspname IS NULL AND defaclnamespace != 0;


-- Grant on all objects of given type in a schema
\c -

CREATE SCHEMA testns;
CREATE TABLE testns.t1 (f1 int);
CREATE TABLE testns.t2 (f1 int);

SELECT has_table_privilege('regress_priv_user1', 'testns.t1', 'SELECT'); -- false

GRANT ALL ON ALL TABLES IN SCHEMA testns TO regress_priv_user1;

SELECT has_table_privilege('regress_priv_user1', 'testns.t1', 'SELECT'); -- true
SELECT has_table_privilege('regress_priv_user1', 'testns.t2', 'SELECT'); -- true

REVOKE ALL ON ALL TABLES IN SCHEMA testns FROM regress_priv_user1;

SELECT has_table_privilege('regress_priv_user1', 'testns.t1', 'SELECT'); -- false
SELECT has_table_privilege('regress_priv_user1', 'testns.t2', 'SELECT'); -- false

CREATE FUNCTION testns.priv_testfunc(int) RETURNS int AS 'select 3 * $1;' LANGUAGE sql;
CREATE AGGREGATE testns.priv_testagg(int) (sfunc = int4pl, stype = int4);
CREATE PROCEDURE testns.priv_testproc(int) AS 'select 3' LANGUAGE sql;

SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testfunc(int)', 'EXECUTE'); -- true by default
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testagg(int)', 'EXECUTE'); -- true by default
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testproc(int)', 'EXECUTE'); -- true by default

REVOKE ALL ON ALL FUNCTIONS IN SCHEMA testns FROM PUBLIC;

SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testfunc(int)', 'EXECUTE'); -- false
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testagg(int)', 'EXECUTE'); -- false
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testproc(int)', 'EXECUTE'); -- still true, not a function

REVOKE ALL ON ALL PROCEDURES IN SCHEMA testns FROM PUBLIC;

SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testproc(int)', 'EXECUTE'); -- now false

GRANT ALL ON ALL ROUTINES IN SCHEMA testns TO PUBLIC;

SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testfunc(int)', 'EXECUTE'); -- true
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testagg(int)', 'EXECUTE'); -- true
SELECT has_function_privilege('regress_priv_user1', 'testns.priv_testproc(int)', 'EXECUTE'); -- true

DROP SCHEMA testns CASCADE;


-- Change owner of the schema & and rename of new schema owner
\c -

CREATE ROLE regress_schemauser1 superuser login;
CREATE ROLE regress_schemauser2 superuser login;

SET SESSION ROLE regress_schemauser1;
CREATE SCHEMA testns;

SELECT nspname, rolname FROM pg_namespace, pg_roles WHERE pg_namespace.nspname = 'testns' AND pg_namespace.nspowner = pg_roles.oid;

ALTER SCHEMA testns OWNER TO regress_schemauser2;
ALTER ROLE regress_schemauser2 RENAME TO regress_schemauser_renamed;
SELECT nspname, rolname FROM pg_namespace, pg_roles WHERE pg_namespace.nspname = 'testns' AND pg_namespace.nspowner = pg_roles.oid;

set session role regress_schemauser_renamed;
DROP SCHEMA testns CASCADE;

-- clean up
\c -

DROP ROLE regress_schemauser1;
DROP ROLE regress_schemauser_renamed;


-- test that dependent privileges are revoked (or not) properly
\c -

set session role regress_priv_user1;
create table dep_priv_test (a int);
grant select on dep_priv_test to regress_priv_user2 with grant option;
grant select on dep_priv_test to regress_priv_user3 with grant option;
set session role regress_priv_user2;
grant select on dep_priv_test to regress_priv_user4 with grant option;
set session role regress_priv_user3;
grant select on dep_priv_test to regress_priv_user4 with grant option;
set session role regress_priv_user4;
grant select on dep_priv_test to regress_priv_user5;
\dp dep_priv_test
set session role regress_priv_user2;
revoke select on dep_priv_test from regress_priv_user4 cascade;
\dp dep_priv_test
set session role regress_priv_user3;
revoke select on dep_priv_test from regress_priv_user4 cascade;
\dp dep_priv_test
set session role regress_priv_user1;
drop table dep_priv_test;


-- clean up

\c

drop sequence x_seq;

DROP AGGREGATE priv_testagg1(int);
DROP FUNCTION priv_testfunc2(int);
DROP FUNCTION priv_testfunc4(boolean);
DROP PROCEDURE priv_testproc1(int);

DROP VIEW atestv0;
DROP VIEW atestv1;
DROP VIEW atestv2;
-- this should cascade to drop atestv4
DROP VIEW atestv3 CASCADE;
-- this should complain "does not exist"
DROP VIEW atestv4;

DROP TABLE atest1;
DROP TABLE atest2;
DROP TABLE atest3;
DROP TABLE atest4;
DROP TABLE atest5;
DROP TABLE atest6;
DROP TABLE atestc;
DROP TABLE atestp1;
DROP TABLE atestp2;

SELECT lo_unlink(oid) FROM pg_largeobject_metadata WHERE oid >= 1000 AND oid < 3000 ORDER BY oid;

DROP GROUP regress_priv_group1;
DROP GROUP regress_priv_group2;

-- these are needed to clean up permissions
REVOKE USAGE ON LANGUAGE sql FROM regress_priv_user1;
DROP OWNED BY regress_priv_user1;

DROP USER regress_priv_user1;
DROP USER regress_priv_user2;
DROP USER regress_priv_user3;
DROP USER regress_priv_user4;
DROP USER regress_priv_user5;
DROP USER regress_priv_user6;


-- permissions with LOCK TABLE
CREATE USER regress_locktable_user;
CREATE TABLE lock_table (a int);

-- LOCK TABLE and SELECT permission
GRANT SELECT ON lock_table TO regress_locktable_user;
SET SESSION AUTHORIZATION regress_locktable_user;
BEGIN;
LOCK TABLE lock_table IN ROW EXCLUSIVE MODE; -- should fail
ROLLBACK;
BEGIN;
LOCK TABLE lock_table IN ACCESS SHARE MODE; -- should pass
COMMIT;
BEGIN;
LOCK TABLE lock_table IN ACCESS EXCLUSIVE MODE; -- should fail
ROLLBACK;
\c
REVOKE SELECT ON lock_table FROM regress_locktable_user;

-- LOCK TABLE and INSERT permission
GRANT INSERT ON lock_table TO regress_locktable_user;
SET SESSION AUTHORIZATION regress_locktable_user;
BEGIN;
LOCK TABLE lock_table IN ROW EXCLUSIVE MODE; -- should pass
COMMIT;
BEGIN;
LOCK TABLE lock_table IN ACCESS SHARE MODE; -- should fail
ROLLBACK;
BEGIN;
LOCK TABLE lock_table IN ACCESS EXCLUSIVE MODE; -- should fail
ROLLBACK;
\c
REVOKE INSERT ON lock_table FROM regress_locktable_user;

-- LOCK TABLE and UPDATE permission
GRANT UPDATE ON lock_table TO regress_locktable_user;
SET SESSION AUTHORIZATION regress_locktable_user;
BEGIN;
LOCK TABLE lock_table IN ROW EXCLUSIVE MODE; -- should pass
COMMIT;
BEGIN;
LOCK TABLE lock_table IN ACCESS SHARE MODE; -- should fail
ROLLBACK;
BEGIN;
LOCK TABLE lock_table IN ACCESS EXCLUSIVE MODE; -- should pass
COMMIT;
\c
REVOKE UPDATE ON lock_table FROM regress_locktable_user;

-- LOCK TABLE and DELETE permission
GRANT DELETE ON lock_table TO regress_locktable_user;
SET SESSION AUTHORIZATION regress_locktable_user;
BEGIN;
LOCK TABLE lock_table IN ROW EXCLUSIVE MODE; -- should pass
COMMIT;
BEGIN;
LOCK TABLE lock_table IN ACCESS SHARE MODE; -- should fail
ROLLBACK;
BEGIN;
LOCK TABLE lock_table IN ACCESS EXCLUSIVE MODE; -- should pass
COMMIT;
\c
REVOKE DELETE ON lock_table FROM regress_locktable_user;

-- LOCK TABLE and TRUNCATE permission
GRANT TRUNCATE ON lock_table TO regress_locktable_user;
SET SESSION AUTHORIZATION regress_locktable_user;
BEGIN;
LOCK TABLE lock_table IN ROW EXCLUSIVE MODE; -- should pass
COMMIT;
BEGIN;
LOCK TABLE lock_table IN ACCESS SHARE MODE; -- should fail
ROLLBACK;
BEGIN;
LOCK TABLE lock_table IN ACCESS EXCLUSIVE MODE; -- should pass
COMMIT;
\c
REVOKE TRUNCATE ON lock_table FROM regress_locktable_user;

-- clean up
DROP TABLE lock_table;
DROP USER regress_locktable_user;
