--
-- JOIN
-- Test join clauses
--

CREATE TABLE JOIN1_TBL (
  i integer,
  j integer,
  t text
);

CREATE TABLE JOIN2_TBL (
  i integer,
  k integer
);

CREATE TABLE JOIN3_TBL (
  i integer,
  j integer,
  y integer
);

CREATE TABLE JOIN4_TBL (
  k integer,
  z integer
);

INSERT INTO JOIN1_TBL VALUES (1, 3, 'one');
INSERT INTO JOIN1_TBL VALUES (2, 2, 'two');
INSERT INTO JOIN1_TBL VALUES (3, 1, 'three');
INSERT INTO JOIN1_TBL VALUES (4, 0, 'four');

INSERT INTO JOIN2_TBL VALUES (1, -1);
INSERT INTO JOIN2_TBL VALUES (2, 2);
INSERT INTO JOIN2_TBL VALUES (3, -3);
INSERT INTO JOIN2_TBL VALUES (2, 4);


--
-- CROSS JOIN
-- Qualifications are not allowed on cross joins,
-- which degenerate into a standard unqualified inner join.
--

SELECT '' AS "xxx", *
  FROM JOIN1_TBL CROSS JOIN JOIN2_TBL;

SELECT '' AS "xxx", i, k, t
  FROM JOIN1_TBL CROSS JOIN JOIN2_TBL;

SELECT '' AS "xxx", ii, tt, kk
  FROM JOIN1_TBL CROSS JOIN JOIN2_TBL AS JT (ii, jj, tt, ii2, kk);

SELECT '' AS "xxx", jt.ii, jt.jj, jt.kk
  FROM JOIN1_TBL CROSS JOIN JOIN2_TBL AS JT (ii, jj, tt, ii2, kk);


--
--
-- Inner joins (equi-joins)
--
--

--
-- Inner joins (equi-joins) with USING clause
-- The USING syntax changes the shape of the resulting table
-- by including a column in the USING clause only once in the result.
--

-- Inner equi-join on all columns with the same name
SELECT '' AS "xxx", *
  FROM JOIN1_TBL NATURAL JOIN JOIN2_TBL;

-- Inner equi-join on specified column
SELECT '' AS "xxx", *
  FROM JOIN1_TBL INNER JOIN JOIN2_TBL USING (i);

-- Same as above, slightly different syntax
SELECT '' AS "xxx", *
  FROM JOIN1_TBL JOIN JOIN2_TBL USING (i);


--
-- Inner joins (equi-joins)
--

SELECT '' AS "xxx", *
  FROM JOIN1_TBL JOIN JOIN2_TBL ON (JOIN1_TBL.i = JOIN2_TBL.i);

SELECT '' AS "xxx", *
  FROM JOIN1_TBL JOIN JOIN2_TBL ON (JOIN1_TBL.i = JOIN2_TBL.k);

SELECT '' AS "xxx", *
  FROM JOIN1_TBL CROSS JOIN JOIN2_TBL;


--
-- Non-equi-joins
--

SELECT '' AS "xxx", *
  FROM JOIN1_TBL JOIN JOIN2_TBL ON (JOIN1_TBL.i <= JOIN2_TBL.k);


--
-- Outer joins
--

SELECT '' AS "xxx", *
  FROM JOIN1_TBL OUTER JOIN JOIN2_TBL USING (i);

SELECT '' AS "xxx", *
  FROM JOIN1_TBL LEFT OUTER JOIN JOIN2_TBL USING (i);

SELECT '' AS "xxx", *
  FROM JOIN1_TBL RIGHT OUTER JOIN JOIN2_TBL USING (i);

SELECT '' AS "xxx", *
  FROM JOIN1_TBL FULL OUTER JOIN JOIN2_TBL USING (i);


--
-- More complicated constructs
--

--
-- Clean up
--

DROP TABLE JOIN1_TBL;
DROP TABLE JOIN2_TBL;

