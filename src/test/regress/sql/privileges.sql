--
-- Test access privileges
--

CREATE USER regressuser1;
CREATE USER regressuser2;
CREATE USER regressuser3;
CREATE USER regressuser4;
CREATE USER regressuser4;	-- duplicate

CREATE GROUP regressgroup1;
CREATE GROUP regressgroup2 WITH USER regressuser1, regressuser2;

ALTER GROUP regressgroup1 ADD USER regressuser4;

ALTER GROUP regressgroup2 ADD USER regressuser2;	-- duplicate
ALTER GROUP regressgroup2 DROP USER regressuser2;
ALTER GROUP regressgroup2 ADD USER regressuser4;


-- test owner privileges

SET SESSION AUTHORIZATION regressuser1;
SELECT session_user, current_user;

CREATE TABLE atest1 ( a int, b text );
SELECT * FROM atest1;
INSERT INTO atest1 VALUES (1, 'one');
DELETE FROM atest1;
UPDATE atest1 SET a = 1 WHERE b = 'blech';
LOCK atest1 IN ACCESS EXCLUSIVE MODE;

REVOKE ALL ON atest1 FROM PUBLIC;
SELECT * FROM atest1;

GRANT ALL ON atest1 TO regressuser2;
GRANT SELECT ON atest1 TO regressuser3, regressuser4;
SELECT * FROM atest1;

CREATE TABLE atest2 (col1 varchar(10), col2 boolean);
GRANT SELECT ON atest2 TO regressuser2;
GRANT UPDATE ON atest2 TO regressuser3;
GRANT INSERT ON atest2 TO regressuser4;


SET SESSION AUTHORIZATION regressuser2;
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
LOCK atest2 IN ACCESS EXCLUSIVE MODE; -- fail
COPY atest2 FROM stdin; -- fail
GRANT ALL ON atest1 TO PUBLIC; -- fail

-- checks in subquery, both ok
SELECT * FROM atest1 WHERE ( b IN ( SELECT col1 FROM atest2 ) );
SELECT * FROM atest2 WHERE ( col1 IN ( SELECT b FROM atest1 ) );


SET SESSION AUTHORIZATION regressuser3;
SELECT session_user, current_user;

SELECT * FROM atest1; -- ok
SELECT * FROM atest2; -- fail
INSERT INTO atest1 VALUES (2, 'two'); -- fail
INSERT INTO atest2 VALUES ('foo', true); -- fail
INSERT INTO atest1 SELECT 1, b FROM atest1; -- fail
UPDATE atest1 SET a = 1 WHERE a = 2; -- fail
UPDATE atest2 SET col2 = NULL; -- ok
UPDATE atest2 SET col2 = NOT col2; -- fails; requires SELECT on atest2
UPDATE atest2 SET col2 = true WHERE atest1.a = 5; -- ok
SELECT * FROM atest1 FOR UPDATE; -- fail
SELECT * FROM atest2 FOR UPDATE; -- fail
DELETE FROM atest2; -- fail
LOCK atest2 IN ACCESS EXCLUSIVE MODE; -- ok
COPY atest2 FROM stdin; -- fail

-- checks in subquery, both fail
SELECT * FROM atest1 WHERE ( b IN ( SELECT col1 FROM atest2 ) );
SELECT * FROM atest2 WHERE ( col1 IN ( SELECT b FROM atest1 ) );

SET SESSION AUTHORIZATION regressuser4;
COPY atest2 FROM stdin; -- ok
bar	true
\.
SELECT * FROM atest1; -- ok


-- groups

SET SESSION AUTHORIZATION regressuser3;
CREATE TABLE atest3 (one int, two int, three int);
GRANT DELETE ON atest3 TO GROUP regressgroup2;

SET SESSION AUTHORIZATION regressuser1;

SELECT * FROM atest3; -- fail
DELETE FROM atest3; -- ok


-- views

SET SESSION AUTHORIZATION regressuser3;

CREATE VIEW atestv1 AS SELECT * FROM atest1; -- ok
/* The next *should* fail, but it's not implemented that way yet. */
CREATE VIEW atestv2 AS SELECT * FROM atest2;
CREATE VIEW atestv3 AS SELECT * FROM atest3; -- ok

SELECT * FROM atestv1; -- ok
SELECT * FROM atestv2; -- fail
GRANT SELECT ON atestv1, atestv3 TO regressuser4;
GRANT SELECT ON atestv2 TO regressuser2;

SET SESSION AUTHORIZATION regressuser4;

SELECT * FROM atestv1; -- ok
SELECT * FROM atestv2; -- fail
SELECT * FROM atestv3; -- ok

CREATE VIEW atestv4 AS SELECT * FROM atestv3; -- nested view
SELECT * FROM atestv4; -- ok
GRANT SELECT ON atestv4 TO regressuser2;

SET SESSION AUTHORIZATION regressuser2;

-- Two complex cases:

SELECT * FROM atestv3; -- fail
SELECT * FROM atestv4; -- ok (even though regressuser2 cannot access underlying atestv3)

SELECT * FROM atest2; -- ok
SELECT * FROM atestv2; -- fail (even though regressuser2 can access underlying atest2)


-- privileges on functions, languages

-- switch to superuser
\c -

REVOKE ALL PRIVILEGES ON LANGUAGE sql FROM PUBLIC;
GRANT USAGE ON LANGUAGE sql TO regressuser1; -- ok
GRANT USAGE ON LANGUAGE c TO PUBLIC; -- fail

SET SESSION AUTHORIZATION regressuser1;
GRANT USAGE ON LANGUAGE sql TO regressuser2; -- fail
CREATE FUNCTION testfunc1(int) RETURNS int AS 'select 2 * $1;' LANGUAGE sql;
CREATE FUNCTION testfunc2(int) RETURNS int AS 'select 3 * $1;' LANGUAGE sql;

REVOKE ALL ON FUNCTION testfunc1(int), testfunc2(int) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION testfunc1(int), testfunc2(int) TO regressuser2;
GRANT USAGE ON FUNCTION testfunc1(int) TO regressuser3; -- semantic error
GRANT ALL PRIVILEGES ON FUNCTION testfunc1(int) TO regressuser4;
GRANT ALL PRIVILEGES ON FUNCTION testfunc_nosuch(int) TO regressuser4;

CREATE FUNCTION testfunc4(boolean) RETURNS text
  AS 'select col1 from atest2 where col2 = $1;'
  LANGUAGE sql SECURITY DEFINER;
GRANT EXECUTE ON FUNCTION testfunc4(boolean) TO regressuser3;

SET SESSION AUTHORIZATION regressuser2;
SELECT testfunc1(5), testfunc2(5); -- ok
CREATE FUNCTION testfunc3(int) RETURNS int AS 'select 2 * $1;' LANGUAGE sql; -- fail

SET SESSION AUTHORIZATION regressuser3;
SELECT testfunc1(5); -- fail
SELECT col1 FROM atest2 WHERE col2 = true; -- fail
SELECT testfunc4(true); -- ok

SET SESSION AUTHORIZATION regressuser4;
SELECT testfunc1(5); -- ok

DROP FUNCTION testfunc1(int); -- fail

\c -

DROP FUNCTION testfunc1(int); -- ok
-- restore to sanity
GRANT ALL PRIVILEGES ON LANGUAGE sql TO PUBLIC;


-- has_table_privilege function

-- bad-input checks
select has_table_privilege(NULL,'pg_shadow','select');
select has_table_privilege('pg_shad','select');
select has_table_privilege('nosuchuser','pg_shadow','select');
select has_table_privilege('pg_shadow','sel');
select has_table_privilege(-999999,'pg_shadow','update');
select has_table_privilege(1,'rule');

-- superuser
\c -

select has_table_privilege(current_user,'pg_shadow','select');
select has_table_privilege(current_user,'pg_shadow','insert');

select has_table_privilege(t2.usesysid,'pg_shadow','update')
from (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,'pg_shadow','delete')
from (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege(current_user,t1.oid,'rule')
from (select oid from pg_class where relname = 'pg_shadow') as t1;
select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'pg_shadow') as t1;

select has_table_privilege(t2.usesysid,t1.oid,'select')
from (select oid from pg_class where relname = 'pg_shadow') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,t1.oid,'insert')
from (select oid from pg_class where relname = 'pg_shadow') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege('pg_shadow','update');
select has_table_privilege('pg_shadow','delete');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'pg_shadow') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'pg_shadow') as t1;

-- non-superuser
SET SESSION AUTHORIZATION regressuser3;

select has_table_privilege(current_user,'pg_class','select');
select has_table_privilege(current_user,'pg_class','insert');

select has_table_privilege(t2.usesysid,'pg_class','update')
from (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,'pg_class','delete')
from (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege(current_user,t1.oid,'rule')
from (select oid from pg_class where relname = 'pg_class') as t1;
select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'pg_class') as t1;

select has_table_privilege(t2.usesysid,t1.oid,'select')
from (select oid from pg_class where relname = 'pg_class') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,t1.oid,'insert')
from (select oid from pg_class where relname = 'pg_class') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege('pg_class','update');
select has_table_privilege('pg_class','delete');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'pg_class') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'pg_class') as t1;

select has_table_privilege(current_user,'atest1','select');
select has_table_privilege(current_user,'atest1','insert');

select has_table_privilege(t2.usesysid,'atest1','update')
from (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,'atest1','delete')
from (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege(current_user,t1.oid,'rule')
from (select oid from pg_class where relname = 'atest1') as t1;
select has_table_privilege(current_user,t1.oid,'references')
from (select oid from pg_class where relname = 'atest1') as t1;

select has_table_privilege(t2.usesysid,t1.oid,'select')
from (select oid from pg_class where relname = 'atest1') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;
select has_table_privilege(t2.usesysid,t1.oid,'insert')
from (select oid from pg_class where relname = 'atest1') as t1,
  (select usesysid from pg_user where usename = current_user) as t2;

select has_table_privilege('atest1','update');
select has_table_privilege('atest1','delete');

select has_table_privilege(t1.oid,'select')
from (select oid from pg_class where relname = 'atest1') as t1;
select has_table_privilege(t1.oid,'trigger')
from (select oid from pg_class where relname = 'atest1') as t1;


-- Grant options

SET SESSION AUTHORIZATION regressuser1;

CREATE TABLE atest4 (a int);

GRANT SELECT ON atest4 TO regressuser2 WITH GRANT OPTION;
GRANT UPDATE ON atest4 TO regressuser2;
GRANT SELECT ON atest4 TO GROUP regressgroup1 WITH GRANT OPTION; -- fail

SET SESSION AUTHORIZATION regressuser2;

GRANT SELECT ON atest4 TO regressuser3;
GRANT UPDATE ON atest4 TO regressuser3; -- fail

SET SESSION AUTHORIZATION regressuser1;

REVOKE SELECT ON atest4 FROM regressuser3; -- does nothing
SELECT has_table_privilege('regressuser3', 'atest4', 'SELECT'); -- true
REVOKE SELECT ON atest4 FROM regressuser2; -- fail
REVOKE GRANT OPTION FOR SELECT ON atest4 FROM regressuser2 CASCADE; -- ok
SELECT has_table_privilege('regressuser2', 'atest4', 'SELECT'); -- true
SELECT has_table_privilege('regressuser3', 'atest4', 'SELECT'); -- false

SELECT has_table_privilege('regressuser1', 'atest4', 'SELECT WITH GRANT OPTION'); -- true


-- clean up

\c regression

DROP FUNCTION testfunc2(int);
DROP FUNCTION testfunc4(boolean);

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

DROP GROUP regressgroup1;
DROP GROUP regressgroup2;

DROP USER regressuser1;
DROP USER regressuser2;
DROP USER regressuser3;
DROP USER regressuser4;
