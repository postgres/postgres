--
-- case.sql
--
-- Test the case statement

--
-- Simplest examples without involving tables
--

SELECT '' AS "One",
  CASE
    WHEN 1 < 2 THEN 3
  END AS "One only = 3";

SELECT '' AS "One",
  CASE
    WHEN 1 > 2 THEN 3
  END AS "One only = Null";

SELECT '' AS "One",
  CASE
    WHEN 1 < 2 THEN 3
    ELSE 4
  END AS "One with default = 3";

SELECT '' AS "One",
  CASE
    WHEN 1 > 2 THEN 3
    ELSE 4
  END AS "One with default = 4";

SELECT '' AS "One",
  CASE
    WHEN 1 > 2 THEN 3
    WHEN 4 < 5 THEN 6
    ELSE 7
  END AS "Two with default = 6";

--
-- Examples of targets involving tables
--

SELECT '' AS "Five",
  CASE
    WHEN f1 >= 0 THEN f1
  END AS ">= 0 or Null"
  FROM INT4_TBL;

SELECT '' AS "Five",
  CASE WHEN f1 >= 0 THEN (f1 - f1)
       ELSE f1
  END AS "Simplest Math"
  FROM INT4_TBL;

SELECT '' AS "Five", f1 AS "Value",
  CASE WHEN (f1 < 0) THEN 'small'
       WHEN (f1 = 0) THEN 'zero'
       WHEN (f1 = 1) THEN 'one'
       WHEN (f1 = 2) THEN 'two'
       ELSE 'big'
  END AS "Category"
  FROM INT4_TBL;

/*
SELECT '' AS "Five",
  CASE WHEN ((f1 < 0) or (i < 0)) THEN 'small'
       WHEN ((f1 = 0) or (i = 0)) THEN 'zero'
       WHEN ((f1 = 1) or (i = 1)) THEN 'one'
       WHEN ((f1 = 2) or (i = 2)) THEN 'two'
       ELSE 'big'
  END AS "Category"
  FROM INT4_TBL;
*/

--
-- Examples of qualifications involving tables
--

