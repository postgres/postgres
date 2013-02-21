-- ===================================================================
-- create FDW objects
-- ===================================================================

CREATE EXTENSION postgres_fdw;

CREATE SERVER testserver1 FOREIGN DATA WRAPPER postgres_fdw;
CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (dbname 'contrib_regression');

CREATE USER MAPPING FOR public SERVER testserver1
	OPTIONS (user 'value', password 'value');
CREATE USER MAPPING FOR CURRENT_USER SERVER loopback;

-- ===================================================================
-- create objects used through FDW loopback server
-- ===================================================================
CREATE TYPE user_enum AS ENUM ('foo', 'bar', 'buz');
CREATE SCHEMA "S 1";
CREATE TABLE "S 1"."T 1" (
	"C 1" int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10),
	c8 user_enum,
	CONSTRAINT t1_pkey PRIMARY KEY ("C 1")
);
CREATE TABLE "S 1"."T 2" (
	c1 int NOT NULL,
	c2 text,
	CONSTRAINT t2_pkey PRIMARY KEY (c1)
);

INSERT INTO "S 1"."T 1"
	SELECT id,
	       id % 10,
	       to_char(id, 'FM00000'),
	       '1970-01-01'::timestamptz + ((id % 100) || ' days')::interval,
	       '1970-01-01'::timestamp + ((id % 100) || ' days')::interval,
	       id % 10,
	       id % 10,
	       'foo'::user_enum
	FROM generate_series(1, 1000) id;
INSERT INTO "S 1"."T 2"
	SELECT id,
	       'AAA' || to_char(id, 'FM000')
	FROM generate_series(1, 100) id;

ANALYZE "S 1"."T 1";
ANALYZE "S 1"."T 2";

-- ===================================================================
-- create foreign tables
-- ===================================================================
CREATE FOREIGN TABLE ft1 (
	c0 int,
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10),
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft1 DROP COLUMN c0;

CREATE FOREIGN TABLE ft2 (
	c0 int,
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10),
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft2 DROP COLUMN c0;

-- ===================================================================
-- tests for validator
-- ===================================================================
-- requiressl, krbsrvname and gsslib are omitted because they depend on
-- configure options
ALTER SERVER testserver1 OPTIONS (
	use_remote_explain 'false',
	fdw_startup_cost '123.456',
	fdw_tuple_cost '0.123',
	service 'value',
	connect_timeout 'value',
	dbname 'value',
	host 'value',
	hostaddr 'value',
	port 'value',
	--client_encoding 'value',
	application_name 'value',
	--fallback_application_name 'value',
	keepalives 'value',
	keepalives_idle 'value',
	keepalives_interval 'value',
	-- requiressl 'value',
	sslcompression 'value',
	sslmode 'value',
	sslcert 'value',
	sslkey 'value',
	sslrootcert 'value',
	sslcrl 'value'
	--requirepeer 'value',
	-- krbsrvname 'value',
	-- gsslib 'value',
	--replication 'value'
);
ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (DROP user, DROP password);
ALTER FOREIGN TABLE ft1 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft2 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
ALTER FOREIGN TABLE ft2 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
\det+

-- Now we should be able to run ANALYZE.
-- To exercise multiple code paths, we use local stats on ft1
-- and remote_explain mode on ft2.
ANALYZE ft1;
ALTER FOREIGN TABLE ft2 OPTIONS (use_remote_explain 'true');

-- ===================================================================
-- simple queries
-- ===================================================================
-- single table, with/without alias
EXPLAIN (COSTS false) SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- empty result
SELECT * FROM ft1 WHERE false;
-- with WHERE clause
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
-- aggregate
SELECT COUNT(*) FROM ft1 t1;
-- join two tables
SELECT t1.c1 FROM ft1 t1 JOIN ft2 t2 ON (t1.c1 = t2.c1) ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- subquery
SELECT * FROM ft1 t1 WHERE t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 <= 10) ORDER BY c1;
-- subquery+MAX
SELECT * FROM ft1 t1 WHERE t1.c3 = (SELECT MAX(c3) FROM ft2 t2) ORDER BY c1;
-- used in CTE
WITH t1 AS (SELECT * FROM ft1 WHERE c1 <= 10) SELECT t2.c1, t2.c2, t2.c3, t2.c4 FROM t1, ft2 t2 WHERE t1.c1 = t2.c1 ORDER BY t1.c1;
-- fixed values
SELECT 'fixed', NULL FROM ft1 t1 WHERE c1 = 1;
-- user-defined operator/function
CREATE FUNCTION postgres_fdw_abs(int) RETURNS int AS $$
BEGIN
RETURN abs($1);
END
$$ LANGUAGE plpgsql IMMUTABLE;
CREATE OPERATOR === (
    LEFTARG = int,
    RIGHTARG = int,
    PROCEDURE = int4eq,
    COMMUTATOR = ===,
    NEGATOR = !==
);
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 === t1.c2;
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = abs(t1.c2);
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = t1.c2;

-- ===================================================================
-- WHERE with remotely-executable conditions
-- ===================================================================
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = 1;         -- Var, OpExpr(b), Const
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = 100 AND t1.c2 = 0; -- BoolExpr
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 IS NULL;        -- NullTest
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 IS NOT NULL;    -- NullTest
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE round(abs(c1), 0) = 1; -- FuncExpr
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 = -c1;          -- OpExpr(l)
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE 1 = c1!;           -- OpExpr(r)
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE (c1 IS NOT NULL) IS DISTINCT FROM (c1 IS NOT NULL); -- DistinctExpr
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 = ANY(ARRAY[c2, 1, c1 + 0]); -- ScalarArrayOpExpr
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 = (ARRAY[c1,c2,3])[1]; -- ArrayRef
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c6 = E'foo''s\\bar';  -- check special chars
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c8 = 'foo';  -- can't be sent to remote

-- ===================================================================
-- parameterized queries
-- ===================================================================
-- simple join
PREPARE st1(int, int) AS SELECT t1.c3, t2.c3 FROM ft1 t1, ft2 t2 WHERE t1.c1 = $1 AND t2.c1 = $2;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st1(1, 2);
EXECUTE st1(1, 1);
EXECUTE st1(101, 101);
-- subquery using stable function (can't be sent to remote)
PREPARE st2(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND EXTRACT(dow FROM c4) = 6) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st2(10, 20);
EXECUTE st2(10, 20);
EXECUTE st1(101, 101);
-- subquery using immutable function (can be sent to remote)
PREPARE st3(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND EXTRACT(dow FROM c5) = 6) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st3(10, 20);
EXECUTE st3(10, 20);
EXECUTE st3(20, 30);
-- custom plan should be chosen initially
PREPARE st4(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 = $1;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
-- once we try it enough times, should switch to generic plan
EXPLAIN (VERBOSE, COSTS false) EXECUTE st4(1);
-- value of $1 should not be sent to remote
PREPARE st5(user_enum,int) AS SELECT * FROM ft1 t1 WHERE c8 = $1 and c1 = $2;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXPLAIN (VERBOSE, COSTS false) EXECUTE st5('foo', 1);
EXECUTE st5('foo', 1);

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;

-- ===================================================================
-- used in pl/pgsql function
-- ===================================================================
CREATE OR REPLACE FUNCTION f_test(p_c1 int) RETURNS int AS $$
DECLARE
	v_c1 int;
BEGIN
    SELECT c1 INTO v_c1 FROM ft1 WHERE c1 = p_c1 LIMIT 1;
    PERFORM c1 FROM ft1 WHERE c1 = p_c1 AND p_c1 = v_c1 LIMIT 1;
    RETURN v_c1;
END;
$$ LANGUAGE plpgsql;
SELECT f_test(100);
DROP FUNCTION f_test(int);

-- ===================================================================
-- conversion error
-- ===================================================================
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE int;
SELECT * FROM ft1 WHERE c1 = 1;  -- ERROR
ALTER FOREIGN TABLE ft1 ALTER COLUMN c8 TYPE user_enum;

-- ===================================================================
-- subtransaction
--  + local/remote error doesn't break cursor
-- ===================================================================
BEGIN;
DECLARE c CURSOR FOR SELECT * FROM ft1 ORDER BY c1;
FETCH c;
SAVEPOINT s;
ERROR OUT;          -- ERROR
ROLLBACK TO s;
FETCH c;
SAVEPOINT s;
SELECT * FROM ft1 WHERE 1 / (c1 - 1) > 0;  -- ERROR
ROLLBACK TO s;
FETCH c;
SELECT * FROM ft1 ORDER BY c1 LIMIT 1;
COMMIT;
