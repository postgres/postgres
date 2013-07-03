--
-- UPDATABLE VIEWS
--

-- check that non-updatable views are rejected with useful error messages

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW ro_view1 AS SELECT DISTINCT a, b FROM base_tbl; -- DISTINCT not supported
CREATE VIEW ro_view2 AS SELECT a, b FROM base_tbl GROUP BY a, b; -- GROUP BY not supported
CREATE VIEW ro_view3 AS SELECT 1 FROM base_tbl HAVING max(a) > 0; -- HAVING not supported
CREATE VIEW ro_view4 AS SELECT count(*) FROM base_tbl; -- Aggregate functions not supported
CREATE VIEW ro_view5 AS SELECT a, rank() OVER() FROM base_tbl; -- Window functions not supported
CREATE VIEW ro_view6 AS SELECT a, b FROM base_tbl UNION SELECT -a, b FROM base_tbl; -- Set ops not supported
CREATE VIEW ro_view7 AS WITH t AS (SELECT a, b FROM base_tbl) SELECT * FROM t; -- WITH not supported
CREATE VIEW ro_view8 AS SELECT a, b FROM base_tbl ORDER BY a OFFSET 1; -- OFFSET not supported
CREATE VIEW ro_view9 AS SELECT a, b FROM base_tbl ORDER BY a LIMIT 1; -- LIMIT not supported
CREATE VIEW ro_view10 AS SELECT 1 AS a; -- No base relations
CREATE VIEW ro_view11 AS SELECT b1.a, b2.b FROM base_tbl b1, base_tbl b2; -- Multiple base relations
CREATE VIEW ro_view12 AS SELECT * FROM generate_series(1, 10) AS g(a); -- SRF in rangetable
CREATE VIEW ro_view13 AS SELECT a, b FROM (SELECT * FROM base_tbl) AS t; -- Subselect in rangetable
CREATE VIEW ro_view14 AS SELECT ctid FROM base_tbl; -- System columns not supported
CREATE VIEW ro_view15 AS SELECT a, upper(b) FROM base_tbl; -- Expression/function in targetlist
CREATE VIEW ro_view16 AS SELECT a, b, a AS aa FROM base_tbl; -- Repeated column
CREATE VIEW ro_view17 AS SELECT * FROM ro_view1; -- Base relation not updatable
CREATE VIEW ro_view18 WITH (security_barrier = true)
  AS SELECT * FROM base_tbl; -- Security barrier views not updatable
CREATE VIEW ro_view19 AS SELECT * FROM (VALUES(1)) AS tmp(a); -- VALUES in rangetable
CREATE SEQUENCE seq;
CREATE VIEW ro_view20 AS SELECT * FROM seq; -- View based on a sequence

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'ro_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'ro_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'ro_view%'
 ORDER BY table_name, ordinal_position;

DELETE FROM ro_view1;
DELETE FROM ro_view2;
DELETE FROM ro_view3;
DELETE FROM ro_view4;
DELETE FROM ro_view5;
DELETE FROM ro_view6;
UPDATE ro_view7 SET a=a+1;
UPDATE ro_view8 SET a=a+1;
UPDATE ro_view9 SET a=a+1;
UPDATE ro_view10 SET a=a+1;
UPDATE ro_view11 SET a=a+1;
UPDATE ro_view12 SET a=a+1;
INSERT INTO ro_view13 VALUES (3, 'Row 3');
INSERT INTO ro_view14 VALUES (null);
INSERT INTO ro_view15 VALUES (3, 'ROW 3');
INSERT INTO ro_view16 VALUES (3, 'Row 3', 3);
INSERT INTO ro_view17 VALUES (3, 'ROW 3');
INSERT INTO ro_view18 VALUES (3, 'ROW 3');
DELETE FROM ro_view19;
UPDATE ro_view20 SET max_value=1000;

DROP TABLE base_tbl CASCADE;
DROP VIEW ro_view10, ro_view12, ro_view19;
DROP SEQUENCE seq CASCADE;

-- simple updatable view

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl WHERE a>0;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name = 'rw_view1';

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name = 'rw_view1';

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name = 'rw_view1'
 ORDER BY ordinal_position;

INSERT INTO rw_view1 VALUES (3, 'Row 3');
INSERT INTO rw_view1 (a) VALUES (4);
UPDATE rw_view1 SET a=5 WHERE a=4;
DELETE FROM rw_view1 WHERE b='Row 2';
SELECT * FROM base_tbl;

EXPLAIN (costs off) UPDATE rw_view1 SET a=6 WHERE a=5;
EXPLAIN (costs off) DELETE FROM rw_view1 WHERE a=5;

DROP TABLE base_tbl CASCADE;

-- view on top of view

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW rw_view1 AS SELECT b AS bb, a AS aa FROM base_tbl WHERE a>0;
CREATE VIEW rw_view2 AS SELECT aa AS aaa, bb AS bbb FROM rw_view1 WHERE aa<10;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name = 'rw_view2';

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name = 'rw_view2';

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name = 'rw_view2'
 ORDER BY ordinal_position;

INSERT INTO rw_view2 VALUES (3, 'Row 3');
INSERT INTO rw_view2 (aaa) VALUES (4);
SELECT * FROM rw_view2;
UPDATE rw_view2 SET bbb='Row 4' WHERE aaa=4;
DELETE FROM rw_view2 WHERE aaa=2;
SELECT * FROM rw_view2;

EXPLAIN (costs off) UPDATE rw_view2 SET aaa=5 WHERE aaa=4;
EXPLAIN (costs off) DELETE FROM rw_view2 WHERE aaa=4;

DROP TABLE base_tbl CASCADE;

-- view on top of view with rules

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl WHERE a>0 OFFSET 0; -- not updatable without rules/triggers
CREATE VIEW rw_view2 AS SELECT * FROM rw_view1 WHERE a<10;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE RULE rw_view1_ins_rule AS ON INSERT TO rw_view1
  DO INSTEAD INSERT INTO base_tbl VALUES (NEW.a, NEW.b) RETURNING *;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE RULE rw_view1_upd_rule AS ON UPDATE TO rw_view1
  DO INSTEAD UPDATE base_tbl SET b=NEW.b WHERE a=OLD.a RETURNING NEW.*;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE RULE rw_view1_del_rule AS ON DELETE TO rw_view1
  DO INSTEAD DELETE FROM base_tbl WHERE a=OLD.a RETURNING OLD.*;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

INSERT INTO rw_view2 VALUES (3, 'Row 3') RETURNING *;
UPDATE rw_view2 SET b='Row three' WHERE a=3 RETURNING *;
SELECT * FROM rw_view2;
DELETE FROM rw_view2 WHERE a=3 RETURNING *;
SELECT * FROM rw_view2;

EXPLAIN (costs off) UPDATE rw_view2 SET a=3 WHERE a=2;
EXPLAIN (costs off) DELETE FROM rw_view2 WHERE a=2;

DROP TABLE base_tbl CASCADE;

-- view on top of view with triggers

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl WHERE a>0 OFFSET 0; -- not updatable without rules/triggers
CREATE VIEW rw_view2 AS SELECT * FROM rw_view1 WHERE a<10;

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into,
       is_trigger_updatable, is_trigger_deletable,
       is_trigger_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE FUNCTION rw_view1_trig_fn()
RETURNS trigger AS
$$
BEGIN
  IF TG_OP = 'INSERT' THEN
    INSERT INTO base_tbl VALUES (NEW.a, NEW.b);
    RETURN NEW;
  ELSIF TG_OP = 'UPDATE' THEN
    UPDATE base_tbl SET b=NEW.b WHERE a=OLD.a;
    RETURN NEW;
  ELSIF TG_OP = 'DELETE' THEN
    DELETE FROM base_tbl WHERE a=OLD.a;
    RETURN OLD;
  END IF;
END;
$$
LANGUAGE plpgsql;

CREATE TRIGGER rw_view1_ins_trig INSTEAD OF INSERT ON rw_view1
  FOR EACH ROW EXECUTE PROCEDURE rw_view1_trig_fn();

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into,
       is_trigger_updatable, is_trigger_deletable,
       is_trigger_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE TRIGGER rw_view1_upd_trig INSTEAD OF UPDATE ON rw_view1
  FOR EACH ROW EXECUTE PROCEDURE rw_view1_trig_fn();

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into,
       is_trigger_updatable, is_trigger_deletable,
       is_trigger_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

CREATE TRIGGER rw_view1_del_trig INSTEAD OF DELETE ON rw_view1
  FOR EACH ROW EXECUTE PROCEDURE rw_view1_trig_fn();

SELECT table_name, is_insertable_into
  FROM information_schema.tables
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, is_updatable, is_insertable_into,
       is_trigger_updatable, is_trigger_deletable,
       is_trigger_insertable_into
  FROM information_schema.views
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name;

SELECT table_name, column_name, is_updatable
  FROM information_schema.columns
 WHERE table_name LIKE 'rw_view%'
 ORDER BY table_name, ordinal_position;

INSERT INTO rw_view2 VALUES (3, 'Row 3') RETURNING *;
UPDATE rw_view2 SET b='Row three' WHERE a=3 RETURNING *;
SELECT * FROM rw_view2;
DELETE FROM rw_view2 WHERE a=3 RETURNING *;
SELECT * FROM rw_view2;

EXPLAIN (costs off) UPDATE rw_view2 SET a=3 WHERE a=2;
EXPLAIN (costs off) DELETE FROM rw_view2 WHERE a=2;

DROP TABLE base_tbl CASCADE;
DROP FUNCTION rw_view1_trig_fn();

-- update using whole row from view

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl SELECT i, 'Row ' || i FROM generate_series(-2, 2) g(i);

CREATE VIEW rw_view1 AS SELECT b AS bb, a AS aa FROM base_tbl;

CREATE FUNCTION rw_view1_aa(x rw_view1)
  RETURNS int AS $$ SELECT x.aa $$ LANGUAGE sql;

UPDATE rw_view1 v SET bb='Updated row 2' WHERE rw_view1_aa(v)=2
  RETURNING rw_view1_aa(v), v.bb;
SELECT * FROM base_tbl;

EXPLAIN (costs off)
UPDATE rw_view1 v SET bb='Updated row 2' WHERE rw_view1_aa(v)=2
  RETURNING rw_view1_aa(v), v.bb;

DROP TABLE base_tbl CASCADE;

-- permissions checks

CREATE USER view_user1;
CREATE USER view_user2;

SET SESSION AUTHORIZATION view_user1;
CREATE TABLE base_tbl(a int, b text, c float);
INSERT INTO base_tbl VALUES (1, 'Row 1', 1.0);
CREATE VIEW rw_view1 AS SELECT b AS bb, c AS cc, a AS aa FROM base_tbl;
INSERT INTO rw_view1 VALUES ('Row 2', 2.0, 2);

GRANT SELECT ON base_tbl TO view_user2;
GRANT SELECT ON rw_view1 TO view_user2;
GRANT UPDATE (a,c) ON base_tbl TO view_user2;
GRANT UPDATE (bb,cc) ON rw_view1 TO view_user2;
RESET SESSION AUTHORIZATION;

SET SESSION AUTHORIZATION view_user2;
CREATE VIEW rw_view2 AS SELECT b AS bb, c AS cc, a AS aa FROM base_tbl;
SELECT * FROM base_tbl; -- ok
SELECT * FROM rw_view1; -- ok
SELECT * FROM rw_view2; -- ok

INSERT INTO base_tbl VALUES (3, 'Row 3', 3.0); -- not allowed
INSERT INTO rw_view1 VALUES ('Row 3', 3.0, 3); -- not allowed
INSERT INTO rw_view2 VALUES ('Row 3', 3.0, 3); -- not allowed

UPDATE base_tbl SET a=a, c=c; -- ok
UPDATE base_tbl SET b=b; -- not allowed
UPDATE rw_view1 SET bb=bb, cc=cc; -- ok
UPDATE rw_view1 SET aa=aa; -- not allowed
UPDATE rw_view2 SET aa=aa, cc=cc; -- ok
UPDATE rw_view2 SET bb=bb; -- not allowed

DELETE FROM base_tbl; -- not allowed
DELETE FROM rw_view1; -- not allowed
DELETE FROM rw_view2; -- not allowed
RESET SESSION AUTHORIZATION;

SET SESSION AUTHORIZATION view_user1;
GRANT INSERT, DELETE ON base_tbl TO view_user2;
RESET SESSION AUTHORIZATION;

SET SESSION AUTHORIZATION view_user2;
INSERT INTO base_tbl VALUES (3, 'Row 3', 3.0); -- ok
INSERT INTO rw_view1 VALUES ('Row 4', 4.0, 4); -- not allowed
INSERT INTO rw_view2 VALUES ('Row 4', 4.0, 4); -- ok
DELETE FROM base_tbl WHERE a=1; -- ok
DELETE FROM rw_view1 WHERE aa=2; -- not allowed
DELETE FROM rw_view2 WHERE aa=2; -- ok
SELECT * FROM base_tbl;
RESET SESSION AUTHORIZATION;

SET SESSION AUTHORIZATION view_user1;
REVOKE INSERT, DELETE ON base_tbl FROM view_user2;
GRANT INSERT, DELETE ON rw_view1 TO view_user2;
RESET SESSION AUTHORIZATION;

SET SESSION AUTHORIZATION view_user2;
INSERT INTO base_tbl VALUES (5, 'Row 5', 5.0); -- not allowed
INSERT INTO rw_view1 VALUES ('Row 5', 5.0, 5); -- ok
INSERT INTO rw_view2 VALUES ('Row 6', 6.0, 6); -- not allowed
DELETE FROM base_tbl WHERE a=3; -- not allowed
DELETE FROM rw_view1 WHERE aa=3; -- ok
DELETE FROM rw_view2 WHERE aa=4; -- not allowed
SELECT * FROM base_tbl;
RESET SESSION AUTHORIZATION;

DROP TABLE base_tbl CASCADE;

DROP USER view_user1;
DROP USER view_user2;

-- column defaults

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified', c serial);
INSERT INTO base_tbl VALUES (1, 'Row 1');
INSERT INTO base_tbl VALUES (2, 'Row 2');
INSERT INTO base_tbl VALUES (3);

CREATE VIEW rw_view1 AS SELECT a AS aa, b AS bb FROM base_tbl;
ALTER VIEW rw_view1 ALTER COLUMN bb SET DEFAULT 'View default';

INSERT INTO rw_view1 VALUES (4, 'Row 4');
INSERT INTO rw_view1 (aa) VALUES (5);

SELECT * FROM base_tbl;

DROP TABLE base_tbl CASCADE;

-- Table having triggers

CREATE TABLE base_tbl (a int PRIMARY KEY, b text DEFAULT 'Unspecified');
INSERT INTO base_tbl VALUES (1, 'Row 1');
INSERT INTO base_tbl VALUES (2, 'Row 2');

CREATE FUNCTION rw_view1_trig_fn()
RETURNS trigger AS
$$
BEGIN
  IF TG_OP = 'INSERT' THEN
    UPDATE base_tbl SET b=NEW.b WHERE a=1;
    RETURN NULL;
  END IF;
  RETURN NULL;
END;
$$
LANGUAGE plpgsql;

CREATE TRIGGER rw_view1_ins_trig AFTER INSERT ON base_tbl
  FOR EACH ROW EXECUTE PROCEDURE rw_view1_trig_fn();

CREATE VIEW rw_view1 AS SELECT a AS aa, b AS bb FROM base_tbl;

INSERT INTO rw_view1 VALUES (3, 'Row 3');
select * from base_tbl;

DROP VIEW rw_view1;
DROP TRIGGER rw_view1_ins_trig on base_tbl;
DROP FUNCTION rw_view1_trig_fn();
DROP TABLE base_tbl;

-- view with ORDER BY

CREATE TABLE base_tbl (a int, b int);
INSERT INTO base_tbl VALUES (1,2), (4,5), (3,-3);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl ORDER BY a+b;

SELECT * FROM rw_view1;

INSERT INTO rw_view1 VALUES (7,-8);
SELECT * FROM rw_view1;

EXPLAIN (verbose, costs off) UPDATE rw_view1 SET b = b + 1 RETURNING *;
UPDATE rw_view1 SET b = b + 1 RETURNING *;
SELECT * FROM rw_view1;

DROP TABLE base_tbl CASCADE;

-- multiple array-column updates

CREATE TABLE base_tbl (a int, arr int[]);
INSERT INTO base_tbl VALUES (1,ARRAY[2]), (3,ARRAY[4]);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl;

UPDATE rw_view1 SET arr[1] = 42, arr[2] = 77 WHERE a = 3;

SELECT * FROM rw_view1;

DROP TABLE base_tbl CASCADE;

-- inheritance tests

CREATE TABLE base_tbl_parent (a int);
CREATE TABLE base_tbl_child (CHECK (a > 0)) INHERITS (base_tbl_parent);
INSERT INTO base_tbl_parent SELECT * FROM generate_series(-8, -1);
INSERT INTO base_tbl_child SELECT * FROM generate_series(1, 8);

CREATE VIEW rw_view1 AS SELECT * FROM base_tbl_parent;
CREATE VIEW rw_view2 AS SELECT * FROM ONLY base_tbl_parent;

SELECT * FROM rw_view1 ORDER BY a;
SELECT * FROM ONLY rw_view1 ORDER BY a;
SELECT * FROM rw_view2 ORDER BY a;

INSERT INTO rw_view1 VALUES (-100), (100);
INSERT INTO rw_view2 VALUES (-200), (200);

UPDATE rw_view1 SET a = a*10 WHERE a IN (-1, 1); -- Should produce -10 and 10
UPDATE ONLY rw_view1 SET a = a*10 WHERE a IN (-2, 2); -- Should produce -20 and 20
UPDATE rw_view2 SET a = a*10 WHERE a IN (-3, 3); -- Should produce -30 only
UPDATE ONLY rw_view2 SET a = a*10 WHERE a IN (-4, 4); -- Should produce -40 only

DELETE FROM rw_view1 WHERE a IN (-5, 5); -- Should delete -5 and 5
DELETE FROM ONLY rw_view1 WHERE a IN (-6, 6); -- Should delete -6 and 6
DELETE FROM rw_view2 WHERE a IN (-7, 7); -- Should delete -7 only
DELETE FROM ONLY rw_view2 WHERE a IN (-8, 8); -- Should delete -8 only

SELECT * FROM ONLY base_tbl_parent ORDER BY a;
SELECT * FROM base_tbl_child ORDER BY a;

DROP TABLE base_tbl_parent, base_tbl_child CASCADE;
