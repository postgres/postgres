--
-- SELECT_INTO
--

SELECT *
   INTO TABLE tmp1
   FROM onek
   WHERE onek.unique1 < 2;

DROP TABLE tmp1;

SELECT *
   INTO TABLE tmp1
   FROM onek2
   WHERE onek2.unique1 < 2;

DROP TABLE tmp1;

--
-- CREATE TABLE AS/SELECT INTO as last command in a SQL function
-- have been known to cause problems
--
CREATE FUNCTION make_table() RETURNS VOID
AS $$
  CREATE TABLE created_table AS SELECT * FROM int8_tbl;
$$ LANGUAGE SQL;

SELECT make_table();

SELECT * FROM created_table;

DROP TABLE created_table;
