-- Regression tests for prepareable statements

PREPARE q1 AS SELECT 1;
EXECUTE q1;

-- should fail
PREPARE q1 AS SELECT 2;

-- should succeed
DEALLOCATE q1;
PREPARE q1 AS SELECT 2;
EXECUTE q1;

-- sql92 syntax
DEALLOCATE PREPARE q1;

-- parameterized queries
PREPARE q2(text) AS
	SELECT datname, datistemplate, datallowconn
	FROM pg_database WHERE datname = $1;
EXECUTE q2('regression');

PREPARE q3(text, int, float, boolean, oid, smallint) AS
	SELECT * FROM tenk1 WHERE string4 = $1 AND (four = $2 OR
	ten = $3::bigint OR true = $4 OR oid = $5 OR odd = $6::int);

EXECUTE q3('AAAAxx', 5::smallint, 10.5::float, false, 500::oid, 4::bigint);

-- too few params
EXECUTE q3('bool');

-- too many params
EXECUTE q3('bytea', 5::smallint, 10.5::float, false, 500::oid, 4::bigint, true);

-- wrong param types
EXECUTE q3(5::smallint, 10.5::float, false, 500::oid, 4::bigint, 'bytea');

-- invalid type
PREPARE q4(nonexistenttype) AS SELECT $1;

-- create table as execute
PREPARE q5(int, text) AS
	SELECT * FROM tenk1 WHERE unique1 = $1 OR stringu1 = $2;
CREATE TEMPORARY TABLE q5_prep_results AS EXECUTE q5(200, 'DTAAAA');
SELECT * FROM q5_prep_results;
