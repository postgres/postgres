--
-- Test INSERT/UPDATE/DELETE RETURNING
--

-- Simple cases

CREATE TEMP TABLE foo (f1 serial, f2 text, f3 int default 42);

INSERT INTO foo (f2,f3)
  VALUES ('test', DEFAULT), ('More', 11), (upper('more'), 7+9)
  RETURNING *, f1+f3 AS sum;

SELECT * FROM foo;

UPDATE foo SET f2 = lower(f2), f3 = DEFAULT RETURNING foo.*, f1+f3 AS sum13;

SELECT * FROM foo;

DELETE FROM foo WHERE f1 > 2 RETURNING f3, f2, f1, least(f1,f3);

SELECT * FROM foo;

-- Subplans and initplans in the RETURNING list

INSERT INTO foo SELECT f1+10, f2, f3+99 FROM foo
  RETURNING *, f1+112 IN (SELECT q1 FROM int8_tbl) AS subplan,
    EXISTS(SELECT * FROM int4_tbl) AS initplan;

UPDATE foo SET f3 = f3 * 2
  WHERE f1 > 10
  RETURNING *, f1+112 IN (SELECT q1 FROM int8_tbl) AS subplan,
    EXISTS(SELECT * FROM int4_tbl) AS initplan;

DELETE FROM foo
  WHERE f1 > 10
  RETURNING *, f1+112 IN (SELECT q1 FROM int8_tbl) AS subplan,
    EXISTS(SELECT * FROM int4_tbl) AS initplan;

-- Joins

UPDATE foo SET f3 = f3*2
  FROM int4_tbl i
  WHERE foo.f1 + 123455 = i.f1
  RETURNING foo.*, i.f1 as "i.f1";

SELECT * FROM foo;

DELETE FROM foo
  USING int4_tbl i
  WHERE foo.f1 + 123455 = i.f1
  RETURNING foo.*, i.f1 as "i.f1";

SELECT * FROM foo;

-- Check inheritance cases

CREATE TEMP TABLE foochild (fc int) INHERITS (foo);

INSERT INTO foochild VALUES(123,'child',999,-123);

ALTER TABLE foo ADD COLUMN f4 int8 DEFAULT 99;

SELECT * FROM foo;
SELECT * FROM foochild;

UPDATE foo SET f4 = f4 + f3 WHERE f4 = 99 RETURNING *;

SELECT * FROM foo;
SELECT * FROM foochild;

UPDATE foo SET f3 = f3*2
  FROM int8_tbl i
  WHERE foo.f1 = i.q1
  RETURNING *;

SELECT * FROM foo;
SELECT * FROM foochild;

DELETE FROM foo
  USING int8_tbl i
  WHERE foo.f1 = i.q1
  RETURNING *;

SELECT * FROM foo;
SELECT * FROM foochild;

DROP TABLE foochild, foo;
