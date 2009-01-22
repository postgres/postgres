CREATE TABLE vutest1 (a integer, b text);
INSERT INTO vutest1 VALUES (1, 'one');
INSERT INTO vutest1 VALUES (2, 'two');


-- simple view updatability conditions

CREATE VIEW vutestv1 AS SELECT a, b FROM vutest1;
CREATE VIEW vutestv2 AS SELECT * FROM vutest1;
CREATE VIEW vutestv3 AS SELECT b, a FROM vutest1;
CREATE VIEW vutestv4 AS SELECT a, b FROM vutest1 WHERE a < 5;

-- not updatable tests:
CREATE VIEW vutestv5 AS SELECT sum(a) FROM vutest1;  -- aggregate function
CREATE VIEW vutestv6 AS SELECT b FROM vutest1 GROUP BY b;  -- GROUP BY
CREATE VIEW vutestv7 AS SELECT l.b AS x, r.b AS y FROM vutest1 l, vutest1 r WHERE r.a = l.a;  -- JOIN
CREATE VIEW vutestv8 AS SELECT 42;  -- no table
CREATE VIEW vutestv9 AS SELECT a * 2 AS x, b || b AS y FROM vutest1;  -- derived columns
CREATE VIEW vutestv10 AS SELECT a AS x, a AS y FROM vutest1;  -- column referenced more than once
CREATE VIEW vutestv11 AS SELECT * FROM generate_series(1, 5);  -- table function
CREATE VIEW vutestv12 AS SELECT xmin, xmax, a, b FROM vutest1;  -- system columns
CREATE VIEW vutestv13 AS SELECT DISTINCT a, b FROM vutest1;  -- DISTINCT
CREATE VIEW vutestv14 AS SELECT a, b FROM vutest1 WHERE a > (SELECT avg(a) FROM vutest1);  -- *is* updatable, but SQL standard disallows this
CREATE VIEW vutestv15 AS SELECT a, b FROM vutest1 UNION ALL SELECT a, b FROM vutest1;  -- UNION
CREATE VIEW vutestv16 AS SELECT x, y FROM (SELECT * FROM vutest1) AS foo (x, y);  -- subquery ("derived table"); SQL standard allows this
CREATE VIEW vutestv17 AS SELECT a, 5, b FROM vutest1;  -- constant
CREATE VIEW vutestv18 AS SELECT a, b FROM vutest1 LIMIT 1;  -- LIMIT
CREATE VIEW vutestv19 AS SELECT a, b FROM vutest1 OFFSET 1;  -- OFFSET
CREATE VIEW vutestv101 AS SELECT a, rank() OVER (PARTITION BY a ORDER BY b DESC) FROM vutest1;  -- window function
CREATE VIEW vutestv102 AS WITH foo AS (SELECT a, b FROM vutest1) SELECT * FROM foo;  -- SQL standard allows this
CREATE VIEW vutestv103 AS WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM t) SELECT a FROM vutest1;  -- recursive

INSERT INTO vutestv1 VALUES (3, 'three');
INSERT INTO vutestv2 VALUES (4, 'four');
INSERT INTO vutestv3 VALUES (5, 'five');  -- fail
INSERT INTO vutestv3 VALUES ('five', 5);
INSERT INTO vutestv3 (a, b) VALUES (6, 'six');
INSERT INTO vutestv4 VALUES (7, 'seven');  -- ok, but would be check option issue
INSERT INTO vutestv5 VALUES (8);  -- fail

SELECT * FROM vutest1;
SELECT * FROM vutestv1;
SELECT * FROM vutestv2;
SELECT * FROM vutestv3;
SELECT * FROM vutestv4;
SELECT * FROM vutestv5;

UPDATE vutestv1 SET b = 'a lot' WHERE a = 7;
DELETE FROM vutestv2 WHERE a = 1;
UPDATE vutestv4 SET b = b || '!' WHERE a > 1;
DELETE FROM vutestv4 WHERE a > 3;
UPDATE vutestv6 SET b = 37; -- fail
DELETE FROM vutestv5;  -- fail

SELECT * FROM vutest1 ORDER BY a, b;
SELECT * FROM vutestv1 ORDER BY a, b;
SELECT * FROM vutestv2 ORDER BY a, b;
SELECT * FROM vutestv4 ORDER BY a, b;

TRUNCATE TABLE vutest1;


-- views on views

CREATE VIEW vutestv20 AS SELECT a AS x, b AS y FROM vutestv1;
CREATE VIEW vutestv21 AS SELECT x AS a FROM vutestv20 WHERE x % 2 = 0;
CREATE VIEW vutestv22 AS SELECT sum(a) FROM vutestv21;  -- not updatable
CREATE VIEW vutestv23 AS SELECT * FROM vutestv12;  -- not updatable

INSERT INTO vutestv20 (x, y) VALUES (1, 'one');
INSERT INTO vutestv20 (x, y) VALUES (3, 'three');
INSERT INTO vutestv21 VALUES (2);

SELECT * FROM vutest1;
SELECT * FROM vutestv20;
SELECT * FROM vutestv21;

UPDATE vutestv20 SET y = 'eins' WHERE x = 1;
UPDATE vutestv21 SET a = 222;

SELECT * FROM vutest1;
SELECT * FROM vutestv20;
SELECT * FROM vutestv21;

DELETE FROM vutestv20 WHERE x = 3;

SELECT * FROM vutest1;
SELECT * FROM vutestv20;
SELECT * FROM vutestv21;


-- insert tests

CREATE TABLE vutest2 (a int PRIMARY KEY, b text NOT NULL, c text NOT NULL DEFAULT 'foo');

CREATE VIEW vutestv30 AS SELECT a, b, c FROM vutest2;
CREATE VIEW vutestv31 AS SELECT a, b FROM vutest2;
CREATE VIEW vutestv32 AS SELECT a, c FROM vutest2;

INSERT INTO vutestv30 VALUES (1, 'one', 'eins');
INSERT INTO vutestv31 VALUES (2, 'two');
INSERT INTO vutestv32 VALUES (3, 'drei');  -- fail

UPDATE vutestv31 SET a = 22 WHERE a = 2;
UPDATE vutestv32 SET c = 'drei!' WHERE a = 3;


SELECT rulename, definition FROM pg_rules WHERE tablename LIKE 'vutestv%' ORDER BY tablename, rulename;


-- interaction of manual and automatic rules, view replacement

CREATE VIEW vutestv40 AS SELECT a, b FROM vutest1;
CREATE RULE zmy_update AS ON UPDATE TO vutestv40 DO INSTEAD DELETE FROM vutest1;  -- drops automatic _UPDATE rule
CREATE RULE "_INSERT" AS ON INSERT TO vutestv40 DO INSTEAD DELETE FROM vutest1;  -- replaces automatic _INSERT rule
CREATE RULE zmy_delete AS ON DELETE TO vutestv40 DO ALSO DELETE FROM vutest1;  -- leaves automatic _DELETE rule (because of ALSO)

CREATE VIEW vutestv41 AS SELECT a + 1 AS aa, b FROM vutest1;  -- not updatable
CREATE RULE "_UPDATE" AS ON UPDATE TO vutestv41 DO INSTEAD UPDATE vutest1 SET a = new.aa - 1, b = new.b WHERE a = old.aa - 1 AND b = old.b;
CREATE OR REPLACE VIEW vutestv41 AS SELECT a AS aa, b FROM vutest1;  -- *now* updatable, manual _UPDATE rule stays

CREATE VIEW vutestv42 AS SELECT a + 1 AS aa, b FROM vutest1;  -- not updatable
CREATE RULE zmy_update AS ON UPDATE TO vutestv42 DO INSTEAD UPDATE vutest1 SET a = new.aa - 1, b = new.b WHERE a = old.aa - 1 AND b = old.b;
CREATE OR REPLACE VIEW vutestv42 AS SELECT a AS aa, b FROM vutest1;  -- *now* updatable, zmy_update stays, no _UPDATE created

CREATE VIEW vutestv43 AS SELECT a AS aa, b FROM vutest1;  -- updatable
CREATE RULE zmy_update AS ON UPDATE TO vutestv43 DO INSTEAD DELETE FROM vutest1;  -- drops automatic _UPDATE rule
CREATE OR REPLACE VIEW vutestv43 AS SELECT a + 1 AS aa, b FROM vutest1;  -- no longer updatable, automatic rules are deleted, manual rules kept

CREATE VIEW vutestv44 AS SELECT a, b FROM vutest1;  -- updatable
CREATE RULE zmy_update AS ON UPDATE TO vutestv44 DO INSTEAD DELETE FROM vutest1;  -- drops automatic _UPDATE rule
CREATE OR REPLACE VIEW vutestv44 AS SELECT a, b FROM vutest2;  -- automatic update rules are updated, manual rules kept


SELECT rulename, definition FROM pg_rules WHERE tablename LIKE 'vutestv4_' ORDER BY tablename, rulename;


-- ACL

CREATE USER regressuser1;
CREATE USER regressuser2;

GRANT SELECT, INSERT, UPDATE ON vutest1 TO regressuser1;

SET ROLE regressuser1;
CREATE VIEW vutestv50 AS SELECT a, b FROM vutest1;

GRANT SELECT, UPDATE, DELETE ON vutestv50 TO regressuser2;

SELECT * FROM vutestv50;
INSERT INTO vutestv50 VALUES (0, 'zero');
UPDATE vutestv50 SET a = 1;
UPDATE vutestv50 SET a = 2 WHERE a = 1;
DELETE FROM vutestv50;  -- ERROR
RESET ROLE;

SET ROLE regressuser2;
SELECT * FROM vutestv50;
INSERT INTO vutestv50 VALUES (0, 'zero');  -- ERROR
UPDATE vutestv50 SET a = 1;
UPDATE vutestv50 SET a = 2 WHERE a = 1;
DELETE FROM vutestv50;  -- ERROR on vutest1
RESET ROLE;

DROP VIEW vutestv50;

REVOKE ALL PRIVILEGES ON vutest1 FROM regressuser1;
DROP USER regressuser1, regressuser2;
