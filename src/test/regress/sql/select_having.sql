--
-- select_having.sql
--

SELECT d1, count(*) FROM DATETIME_TBL
  GROUP BY d1 HAVING count(*) > 1;

