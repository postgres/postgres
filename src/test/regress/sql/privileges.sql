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
GRANT SELECT ON atest1 TO regressuser3;
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
GRANT SELECT ON atestv1 TO regressuser4;
GRANT SELECT ON atestv3 TO regressuser4;

SET SESSION AUTHORIZATION regressuser4;

SELECT * FROM atestv1; -- ok
SELECT * FROM atestv3; -- ok


-- clean up

\c regression
DROP TABLE atest1;
DROP TABLE atest2;
DROP TABLE atest3;

DROP VIEW atestv1;
DROP VIEW atestv2;
DROP VIEW atestv3;

DROP GROUP regressgroup1;
DROP GROUP regressgroup2;

DROP USER regressuser1;
DROP USER regressuser2;
DROP USER regressuser3;
DROP USER regressuser4;
