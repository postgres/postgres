--
-- Tests for common table expressions (WITH query, ... SELECT ...)
--

-- Basic WITH
WITH q1(x,y) AS (SELECT 1,2)
SELECT * FROM q1, q1 AS q2;

-- Multiple uses are evaluated only once
SELECT count(*) FROM (
  WITH q1(x) AS (SELECT random() FROM generate_series(1, 5))
    SELECT * FROM q1
  UNION
    SELECT * FROM q1
) ss;

-- WITH RECURSIVE

-- sum of 1..100
WITH RECURSIVE t(n) AS (
    VALUES (1)
UNION ALL
    SELECT n+1 FROM t WHERE n < 100
)
SELECT sum(n) FROM t;

WITH RECURSIVE t(n) AS (
    SELECT (VALUES(1))
UNION ALL
    SELECT n+1 FROM t WHERE n < 5
)
SELECT * FROM t;

-- This'd be an infinite loop, but outside query reads only as much as needed
WITH RECURSIVE t(n) AS (
    VALUES (1)
UNION ALL
    SELECT n+1 FROM t)
SELECT * FROM t LIMIT 10;

--
-- Some examples with a tree
--
-- department structure represented here is as follows:
--
-- ROOT-+->A-+->B-+->C
--      |         |
--      |         +->D-+->F
--      +->E-+->G

CREATE TEMP TABLE department (
	id INTEGER PRIMARY KEY,  -- department ID
	parent_department INTEGER REFERENCES department, -- upper department ID
	name TEXT -- department name
);

INSERT INTO department VALUES (0, NULL, 'ROOT');
INSERT INTO department VALUES (1, 0, 'A');
INSERT INTO department VALUES (2, 1, 'B');
INSERT INTO department VALUES (3, 2, 'C');
INSERT INTO department VALUES (4, 2, 'D');
INSERT INTO department VALUES (5, 0, 'E');
INSERT INTO department VALUES (6, 4, 'F');
INSERT INTO department VALUES (7, 5, 'G');


-- extract all departments under 'A'. Result should be A, B, C, D and F
WITH RECURSIVE subdepartment AS
(
	-- non recursive term
	SELECT * FROM department WHERE name = 'A'

	UNION ALL

	-- recursive term
	SELECT d.* FROM department AS d, subdepartment AS sd
		WHERE d.parent_department = sd.id
)
SELECT * FROM subdepartment ORDER BY name;

-- extract all departments under 'A' with "level" number
WITH RECURSIVE subdepartment(level, id, parent_department, name) AS
(
	-- non recursive term
	SELECT 1, * FROM department WHERE name = 'A'

	UNION ALL

	-- recursive term
	SELECT sd.level + 1, d.* FROM department AS d, subdepartment AS sd
		WHERE d.parent_department = sd.id
)
SELECT * FROM subdepartment ORDER BY name;

-- extract all departments under 'A' with "level" number.
-- Only shows level 2 or more
WITH RECURSIVE subdepartment(level, id, parent_department, name) AS
(
	-- non recursive term
	SELECT 1, * FROM department WHERE name = 'A'

	UNION ALL

	-- recursive term
	SELECT sd.level + 1, d.* FROM department AS d, subdepartment AS sd
		WHERE d.parent_department = sd.id
)
SELECT * FROM subdepartment WHERE level >= 2 ORDER BY name;

-- "RECURSIVE" is ignored if the query has no self-reference
WITH RECURSIVE subdepartment AS
(
	-- note lack of recursive UNION structure
	SELECT * FROM department WHERE name = 'A'
)
SELECT * FROM subdepartment ORDER BY name;

-- inside subqueries
SELECT count(*) FROM (
    WITH RECURSIVE t(n) AS (
        SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 500
    )
    SELECT * FROM t) AS t WHERE n < (
        SELECT count(*) FROM (
            WITH RECURSIVE t(n) AS (
                   SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 100
                )
            SELECT * FROM t WHERE n < 50000
         ) AS t WHERE n < 100);

-- use same CTE twice at different subquery levels
WITH q1(x,y) AS (
    SELECT hundred, sum(ten) FROM tenk1 GROUP BY hundred
  )
SELECT count(*) FROM q1 WHERE y > (SELECT sum(y)/100 FROM q1 qsub);

-- via a VIEW
CREATE TEMPORARY VIEW vsubdepartment AS
	WITH RECURSIVE subdepartment AS
	(
		 -- non recursive term
		SELECT * FROM department WHERE name = 'A'
		UNION ALL
		-- recursive term
		SELECT d.* FROM department AS d, subdepartment AS sd
			WHERE d.parent_department = sd.id
	)
	SELECT * FROM subdepartment;

SELECT * FROM vsubdepartment ORDER BY name;

-- Check reverse listing
SELECT pg_get_viewdef('vsubdepartment'::regclass);
SELECT pg_get_viewdef('vsubdepartment'::regclass, true);

-- recursive term has sub-UNION
WITH RECURSIVE t(i,j) AS (
	VALUES (1,2)
	UNION ALL
	SELECT t2.i, t.j+1 FROM
		(SELECT 2 AS i UNION ALL SELECT 3 AS i) AS t2
		JOIN t ON (t2.i = t.i+1))

	SELECT * FROM t;

--
-- different tree example
--
CREATE TEMPORARY TABLE tree(
    id INTEGER PRIMARY KEY,
    parent_id INTEGER REFERENCES tree(id)
);

INSERT INTO tree
VALUES (1, NULL), (2, 1), (3,1), (4,2), (5,2), (6,2), (7,3), (8,3),
       (9,4), (10,4), (11,7), (12,7), (13,7), (14, 9), (15,11), (16,11);

--
-- get all paths from "second level" nodes to leaf nodes
--
WITH RECURSIVE t(id, path) AS (
    VALUES(1,ARRAY[]::integer[])
UNION ALL
    SELECT tree.id, t.path || tree.id
    FROM tree JOIN t ON (tree.parent_id = t.id)
)
SELECT t1.*, t2.* FROM t AS t1 JOIN t AS t2 ON
	(t1.path[1] = t2.path[1] AND
	array_upper(t1.path,1) = 1 AND
	array_upper(t2.path,1) > 1)
	ORDER BY t1.id, t2.id;

-- just count 'em
WITH RECURSIVE t(id, path) AS (
    VALUES(1,ARRAY[]::integer[])
UNION ALL
    SELECT tree.id, t.path || tree.id
    FROM tree JOIN t ON (tree.parent_id = t.id)
)
SELECT t1.id, count(t2.*) FROM t AS t1 JOIN t AS t2 ON
	(t1.path[1] = t2.path[1] AND
	array_upper(t1.path,1) = 1 AND
	array_upper(t2.path,1) > 1)
	GROUP BY t1.id
	ORDER BY t1.id;

--
-- test multiple WITH queries
--
WITH RECURSIVE
  y (id) AS (VALUES (1)),
  x (id) AS (SELECT * FROM y UNION ALL SELECT id+1 FROM x WHERE id < 5)
SELECT * FROM x;

-- forward reference OK
WITH RECURSIVE
    x(id) AS (SELECT * FROM y UNION ALL SELECT id+1 FROM x WHERE id < 5),
    y(id) AS (values (1))
 SELECT * FROM x;

WITH RECURSIVE
   x(id) AS
     (VALUES (1) UNION ALL SELECT id+1 FROM x WHERE id < 5),
   y(id) AS
     (VALUES (1) UNION ALL SELECT id+1 FROM y WHERE id < 10)
 SELECT y.*, x.* FROM y LEFT JOIN x USING (id);

WITH RECURSIVE
   x(id) AS
     (VALUES (1) UNION ALL SELECT id+1 FROM x WHERE id < 5),
   y(id) AS
     (VALUES (1) UNION ALL SELECT id+1 FROM x WHERE id < 10)
 SELECT y.*, x.* FROM y LEFT JOIN x USING (id);

WITH RECURSIVE
   x(id) AS
     (SELECT 1 UNION ALL SELECT id+1 FROM x WHERE id < 3 ),
   y(id) AS
     (SELECT * FROM x UNION ALL SELECT * FROM x),
   z(id) AS
     (SELECT * FROM x UNION ALL SELECT id+1 FROM z WHERE id < 10)
 SELECT * FROM z;

WITH RECURSIVE
   x(id) AS
     (SELECT 1 UNION ALL SELECT id+1 FROM x WHERE id < 3 ),
   y(id) AS
     (SELECT * FROM x UNION ALL SELECT * FROM x),
   z(id) AS
     (SELECT * FROM y UNION ALL SELECT id+1 FROM z WHERE id < 10)
 SELECT * FROM z;

--
-- error cases
--

-- UNION (should be supported someday)
WITH RECURSIVE x(n) AS (SELECT 1 UNION SELECT n+1 FROM x)
	SELECT * FROM x;

-- INTERSECT
WITH RECURSIVE x(n) AS (SELECT 1 INTERSECT SELECT n+1 FROM x)
	SELECT * FROM x;

WITH RECURSIVE x(n) AS (SELECT 1 INTERSECT ALL SELECT n+1 FROM x)
	SELECT * FROM x;

-- EXCEPT
WITH RECURSIVE x(n) AS (SELECT 1 EXCEPT SELECT n+1 FROM x)
	SELECT * FROM x;

WITH RECURSIVE x(n) AS (SELECT 1 EXCEPT ALL SELECT n+1 FROM x)
	SELECT * FROM x;

-- no non-recursive term
WITH RECURSIVE x(n) AS (SELECT n FROM x)
	SELECT * FROM x;

-- recursive term in the left hand side (strictly speaking, should allow this)
WITH RECURSIVE x(n) AS (SELECT n FROM x UNION ALL SELECT 1)
	SELECT * FROM x;

CREATE TEMPORARY TABLE y (a INTEGER);
INSERT INTO y SELECT generate_series(1, 10);

-- LEFT JOIN

WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM y LEFT JOIN x ON x.n = y.a WHERE n < 10)
SELECT * FROM x;

-- RIGHT JOIN
WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM x RIGHT JOIN y ON x.n = y.a WHERE n < 10)
SELECT * FROM x;

-- FULL JOIN
WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM x FULL JOIN y ON x.n = y.a WHERE n < 10)
SELECT * FROM x;

-- subquery
WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x
                          WHERE n IN (SELECT * FROM x))
  SELECT * FROM x;

-- aggregate functions
WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT count(*) FROM x)
  SELECT * FROM x;

WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT sum(n) FROM x)
  SELECT * FROM x;

-- ORDER BY
WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x ORDER BY 1)
  SELECT * FROM x;

-- LIMIT/OFFSET
WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x LIMIT 10 OFFSET 1)
  SELECT * FROM x;

-- FOR UPDATE
WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x FOR UPDATE)
  SELECT * FROM x;

-- target list has a recursive query name
WITH RECURSIVE x(id) AS (values (1)
    UNION ALL
    SELECT (SELECT * FROM x) FROM x WHERE id < 5
) SELECT * FROM x;

-- mutual recursive query (not implemented)
WITH RECURSIVE
  x (id) AS (SELECT 1 UNION ALL SELECT id+1 FROM y WHERE id < 5),
  y (id) AS (SELECT 1 UNION ALL SELECT id+1 FROM x WHERE id < 5)
SELECT * FROM x;

-- non-linear recursion is not allowed
WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          UNION ALL
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;

WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
	   SELECT * FROM
       (SELECT i+1 FROM foo WHERE i < 10
          UNION ALL
       SELECT i+1 FROM foo WHERE i < 5) AS t
) SELECT * FROM foo;

WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          EXCEPT
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;

WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          INTERSECT
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;

-- Wrong type induced from non-recursive term
WITH RECURSIVE foo(i) AS
   (SELECT i FROM (VALUES(1),(2)) t(i)
   UNION ALL
   SELECT (i+1)::numeric(10,0) FROM foo WHERE i < 10)
SELECT * FROM foo;

-- rejects different typmod, too (should we allow this?)
WITH RECURSIVE foo(i) AS
   (SELECT i::numeric(3,0) FROM (VALUES(1),(2)) t(i)
   UNION ALL
   SELECT (i+1)::numeric(10,0) FROM foo WHERE i < 10)
SELECT * FROM foo;
