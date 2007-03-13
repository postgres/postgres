--
-- Tests to exercise the plan caching/invalidation mechanism
--

CREATE TEMP TABLE foo AS SELECT * FROM int8_tbl;

-- create and use a cached plan
PREPARE prepstmt AS SELECT * FROM foo;

EXECUTE prepstmt;

-- and one with parameters
PREPARE prepstmt2(bigint) AS SELECT * FROM foo WHERE q1 = $1;

EXECUTE prepstmt2(123);

-- invalidate the plans and see what happens
DROP TABLE foo;

EXECUTE prepstmt;
EXECUTE prepstmt2(123);

-- recreate the temp table (this demonstrates that the raw plan is
-- purely textual and doesn't depend on OIDs, for instance)
CREATE TEMP TABLE foo AS SELECT * FROM int8_tbl ORDER BY 2;

EXECUTE prepstmt;
EXECUTE prepstmt2(123);

-- prepared statements should prevent change in output tupdesc,
-- since clients probably aren't expecting that to change on the fly
ALTER TABLE foo ADD COLUMN q3 bigint;

EXECUTE prepstmt;
EXECUTE prepstmt2(123);

-- but we're nice guys and will let you undo your mistake
ALTER TABLE foo DROP COLUMN q3;

EXECUTE prepstmt;
EXECUTE prepstmt2(123);

-- Try it with a view, which isn't directly used in the resulting plan
-- but should trigger invalidation anyway
CREATE TEMP VIEW voo AS SELECT * FROM foo;

PREPARE vprep AS SELECT * FROM voo;

EXECUTE vprep;

CREATE OR REPLACE TEMP VIEW voo AS SELECT q1, q2/2 AS q2 FROM foo;

EXECUTE vprep;
