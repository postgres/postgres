-- ===================================================================
-- create FDW objects
-- ===================================================================

CREATE EXTENSION postgres_fdw;

CREATE SERVER testserver1 FOREIGN DATA WRAPPER postgres_fdw;
DO $d$
    BEGIN
        EXECUTE $$CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (dbname '$$||current_database()||$$',
                     port '$$||current_setting('port')||$$'
            )$$;
    END;
$d$;

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
	c7 char(10) default 'ft1',
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft1 DROP COLUMN c0;

CREATE FOREIGN TABLE ft2 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	cx int,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft2',
	c8 user_enum
) SERVER loopback;
ALTER FOREIGN TABLE ft2 DROP COLUMN cx;

-- ===================================================================
-- tests for validator
-- ===================================================================
-- requiressl, krbsrvname and gsslib are omitted because they depend on
-- configure options
ALTER SERVER testserver1 OPTIONS (
	use_remote_estimate 'false',
	updatable 'true',
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

-- Error, invalid list syntax
ALTER SERVER testserver1 OPTIONS (ADD extensions 'foo; bar');

-- OK but gets a warning
ALTER SERVER testserver1 OPTIONS (ADD extensions 'foo, bar');
ALTER SERVER testserver1 OPTIONS (DROP extensions);

ALTER USER MAPPING FOR public SERVER testserver1
	OPTIONS (DROP user, DROP password);

ALTER FOREIGN TABLE ft1 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft2 OPTIONS (schema_name 'S 1', table_name 'T 1');
ALTER FOREIGN TABLE ft1 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
ALTER FOREIGN TABLE ft2 ALTER COLUMN c1 OPTIONS (column_name 'C 1');
\det+

-- Now we should be able to run ANALYZE.
-- To exercise multiple code paths, we use local stats on ft1
-- and remote-estimate mode on ft2.
ANALYZE ft1;
ALTER FOREIGN TABLE ft2 OPTIONS (use_remote_estimate 'true');

-- ===================================================================
-- simple queries
-- ===================================================================
-- single table without alias
EXPLAIN (COSTS false) SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
-- single table with alias - also test that tableoid sort is not pushed to remote side
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;
SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;
-- whole-row reference
EXPLAIN (VERBOSE, COSTS false) SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;
-- empty result
SELECT * FROM ft1 WHERE false;
-- with WHERE clause
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';
-- with FOR UPDATE/SHARE
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 = 101 FOR UPDATE;
SELECT * FROM ft1 t1 WHERE c1 = 101 FOR UPDATE;
EXPLAIN (VERBOSE, COSTS false) SELECT * FROM ft1 t1 WHERE c1 = 102 FOR SHARE;
SELECT * FROM ft1 t1 WHERE c1 = 102 FOR SHARE;
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
-- Test forcing the remote server to produce sorted data for a merge join.
SET enable_hashjoin TO false;
SET enable_nestloop TO false;
-- inner join; expressions in the clauses appear in the equivalence class list
EXPLAIN (VERBOSE, COSTS false)
	SELECT t1.c1, t2."C 1" FROM ft2 t1 JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
SELECT t1.c1, t2."C 1" FROM ft2 t1 JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
-- outer join; expressions in the clauses do not appear in equivalence class
-- list but no output change as compared to the previous query
EXPLAIN (VERBOSE, COSTS false)
	SELECT t1.c1, t2."C 1" FROM ft2 t1 LEFT JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
SELECT t1.c1, t2."C 1" FROM ft2 t1 LEFT JOIN "S 1"."T 1" t2 ON (t1.c1 = t2."C 1") OFFSET 100 LIMIT 10;
RESET enable_hashjoin;
RESET enable_nestloop;

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
-- parameterized remote path
EXPLAIN (VERBOSE, COSTS false)
  SELECT * FROM ft2 a, ft2 b WHERE a.c1 = 47 AND b.c1 = a.c2;
SELECT * FROM ft2 a, ft2 b WHERE a.c1 = 47 AND b.c1 = a.c2;
-- check both safe and unsafe join conditions
EXPLAIN (VERBOSE, COSTS false)
  SELECT * FROM ft2 a, ft2 b
  WHERE a.c2 = 6 AND b.c1 = a.c1 AND a.c8 = 'foo' AND b.c7 = upper(a.c7);
SELECT * FROM ft2 a, ft2 b
WHERE a.c2 = 6 AND b.c1 = a.c1 AND a.c8 = 'foo' AND b.c7 = upper(a.c7);
-- bug before 9.3.5 due to sloppy handling of remote-estimate parameters
SELECT * FROM ft1 WHERE c1 = ANY (ARRAY(SELECT c1 FROM ft2 WHERE c1 < 5));
SELECT * FROM ft2 WHERE c1 = ANY (ARRAY(SELECT c1 FROM ft1 WHERE c1 < 5));
-- we should not push order by clause with volatile expressions or unsafe
-- collations
EXPLAIN (VERBOSE, COSTS false)
	SELECT * FROM ft2 ORDER BY ft2.c1, random();
EXPLAIN (VERBOSE, COSTS false)
	SELECT * FROM ft2 ORDER BY ft2.c1, ft2.c3 collate "C";

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
    COMMUTATOR = ===
);

-- built-in operators and functions can be shipped for remote execution
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = abs(t1.c2);
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = t1.c2;

-- by default, user-defined ones cannot
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;

-- but let's put them in an extension ...
ALTER EXTENSION postgres_fdw ADD FUNCTION postgres_fdw_abs(int);
ALTER EXTENSION postgres_fdw ADD OPERATOR === (int, int);
ALTER SERVER loopback OPTIONS (ADD extensions 'postgres_fdw');

-- ... now they can be shipped
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 = postgres_fdw_abs(t1.c2);
EXPLAIN (VERBOSE, COSTS false)
  SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;
SELECT count(c3) FROM ft1 t1 WHERE t1.c1 === t1.c2;

-- ===================================================================
-- parameterized queries
-- ===================================================================
-- simple join
PREPARE st1(int, int) AS SELECT t1.c3, t2.c3 FROM ft1 t1, ft2 t2 WHERE t1.c1 = $1 AND t2.c1 = $2;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st1(1, 2);
EXECUTE st1(1, 1);
EXECUTE st1(101, 101);
-- subquery using stable function (can't be sent to remote)
PREPARE st2(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND date(c4) = '1970-01-17'::date) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS false) EXECUTE st2(10, 20);
EXECUTE st2(10, 20);
EXECUTE st2(101, 121);
-- subquery using immutable function (can be sent to remote)
PREPARE st3(int) AS SELECT * FROM ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM ft2 t2 WHERE c1 > $1 AND date(c5) = '1970-01-17'::date) ORDER BY c1;
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

-- System columns, except ctid, should not be sent to remote
EXPLAIN (VERBOSE, COSTS false)
SELECT * FROM ft1 t1 WHERE t1.tableoid = 'pg_class'::regclass LIMIT 1;
SELECT * FROM ft1 t1 WHERE t1.tableoid = 'ft1'::regclass LIMIT 1;
EXPLAIN (VERBOSE, COSTS false)
SELECT tableoid::regclass, * FROM ft1 t1 LIMIT 1;
SELECT tableoid::regclass, * FROM ft1 t1 LIMIT 1;
EXPLAIN (VERBOSE, COSTS false)
SELECT * FROM ft1 t1 WHERE t1.ctid = '(0,2)';
SELECT * FROM ft1 t1 WHERE t1.ctid = '(0,2)';
EXPLAIN (VERBOSE, COSTS false)
SELECT ctid, * FROM ft1 t1 LIMIT 1;
SELECT ctid, * FROM ft1 t1 LIMIT 1;

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

-- ===================================================================
-- test handling of collations
-- ===================================================================
create table loct3 (f1 text collate "C" unique, f2 text, f3 varchar(10) unique);
create foreign table ft3 (f1 text collate "C", f2 text, f3 varchar(10))
  server loopback options (table_name 'loct3', use_remote_estimate 'true');

-- can be sent to remote
explain (verbose, costs off) select * from ft3 where f1 = 'foo';
explain (verbose, costs off) select * from ft3 where f1 COLLATE "C" = 'foo';
explain (verbose, costs off) select * from ft3 where f2 = 'foo';
explain (verbose, costs off) select * from ft3 where f3 = 'foo';
explain (verbose, costs off) select * from ft3 f, loct3 l
  where f.f3 = l.f3 and l.f1 = 'foo';
-- can't be sent to remote
explain (verbose, costs off) select * from ft3 where f1 COLLATE "POSIX" = 'foo';
explain (verbose, costs off) select * from ft3 where f1 = 'foo' COLLATE "C";
explain (verbose, costs off) select * from ft3 where f2 COLLATE "C" = 'foo';
explain (verbose, costs off) select * from ft3 where f2 = 'foo' COLLATE "C";
explain (verbose, costs off) select * from ft3 f, loct3 l
  where f.f3 = l.f3 COLLATE "POSIX" and l.f1 = 'foo';

-- ===================================================================
-- test writable foreign table stuff
-- ===================================================================
EXPLAIN (verbose, costs off)
INSERT INTO ft2 (c1,c2,c3) SELECT c1+1000,c2+100, c3 || c3 FROM ft2 LIMIT 20;
INSERT INTO ft2 (c1,c2,c3) SELECT c1+1000,c2+100, c3 || c3 FROM ft2 LIMIT 20;
INSERT INTO ft2 (c1,c2,c3)
  VALUES (1101,201,'aaa'), (1102,202,'bbb'), (1103,203,'ccc') RETURNING *;
INSERT INTO ft2 (c1,c2,c3) VALUES (1104,204,'ddd'), (1105,205,'eee');
UPDATE ft2 SET c2 = c2 + 300, c3 = c3 || '_update3' WHERE c1 % 10 = 3;
UPDATE ft2 SET c2 = c2 + 400, c3 = c3 || '_update7' WHERE c1 % 10 = 7 RETURNING *;
EXPLAIN (verbose, costs off)
UPDATE ft2 SET c2 = ft2.c2 + 500, c3 = ft2.c3 || '_update9', c7 = DEFAULT
  FROM ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 9;
UPDATE ft2 SET c2 = ft2.c2 + 500, c3 = ft2.c3 || '_update9', c7 = DEFAULT
  FROM ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 9;
EXPLAIN (verbose, costs off)
  DELETE FROM ft2 WHERE c1 % 10 = 5 RETURNING c1, c4;
DELETE FROM ft2 WHERE c1 % 10 = 5 RETURNING c1, c4;
EXPLAIN (verbose, costs off)
DELETE FROM ft2 USING ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 2;
DELETE FROM ft2 USING ft1 WHERE ft1.c1 = ft2.c2 AND ft1.c1 % 10 = 2;
SELECT c1,c2,c3,c4 FROM ft2 ORDER BY c1;

-- Test that trigger on remote table works as expected
CREATE OR REPLACE FUNCTION "S 1".F_BRTRIG() RETURNS trigger AS $$
BEGIN
    NEW.c3 = NEW.c3 || '_trig_update';
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
CREATE TRIGGER t1_br_insert BEFORE INSERT OR UPDATE
    ON "S 1"."T 1" FOR EACH ROW EXECUTE PROCEDURE "S 1".F_BRTRIG();

INSERT INTO ft2 (c1,c2,c3) VALUES (1208, 818, 'fff') RETURNING *;
INSERT INTO ft2 (c1,c2,c3,c6) VALUES (1218, 818, 'ggg', '(--;') RETURNING *;
UPDATE ft2 SET c2 = c2 + 600 WHERE c1 % 10 = 8 AND c1 < 1200 RETURNING *;

-- Test errors thrown on remote side during update
ALTER TABLE "S 1"."T 1" ADD CONSTRAINT c2positive CHECK (c2 >= 0);

INSERT INTO ft1(c1, c2) VALUES(11, 12);  -- duplicate key
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT DO NOTHING; -- works
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT (c1, c2) DO NOTHING; -- unsupported
INSERT INTO ft1(c1, c2) VALUES(11, 12) ON CONFLICT (c1, c2) DO UPDATE SET c3 = 'ffg'; -- unsupported
INSERT INTO ft1(c1, c2) VALUES(1111, -2);  -- c2positive
UPDATE ft1 SET c2 = -c2 WHERE c1 = 1;  -- c2positive

-- Test savepoint/rollback behavior
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;
begin;
update ft2 set c2 = 42 where c2 = 0;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s1;
update ft2 set c2 = 44 where c2 = 4;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s1;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s2;
update ft2 set c2 = 46 where c2 = 6;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
rollback to savepoint s2;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s2;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
savepoint s3;
update ft2 set c2 = -2 where c2 = 42 and c1 = 10; -- fail on remote side
rollback to savepoint s3;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
release savepoint s3;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
-- none of the above is committed yet remotely
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;
commit;
select c2, count(*) from ft2 where c2 < 500 group by 1 order by 1;
select c2, count(*) from "S 1"."T 1" where c2 < 500 group by 1 order by 1;

-- ===================================================================
-- test check constraints
-- ===================================================================

-- Consistent check constraints provide consistent results
ALTER FOREIGN TABLE ft1 ADD CONSTRAINT ft1_c2positive CHECK (c2 >= 0);
EXPLAIN (VERBOSE, COSTS false) SELECT count(*) FROM ft1 WHERE c2 < 0;
SELECT count(*) FROM ft1 WHERE c2 < 0;
SET constraint_exclusion = 'on';
EXPLAIN (VERBOSE, COSTS false) SELECT count(*) FROM ft1 WHERE c2 < 0;
SELECT count(*) FROM ft1 WHERE c2 < 0;
RESET constraint_exclusion;
-- check constraint is enforced on the remote side, not locally
INSERT INTO ft1(c1, c2) VALUES(1111, -2);  -- c2positive
UPDATE ft1 SET c2 = -c2 WHERE c1 = 1;  -- c2positive
ALTER FOREIGN TABLE ft1 DROP CONSTRAINT ft1_c2positive;

-- But inconsistent check constraints provide inconsistent results
ALTER FOREIGN TABLE ft1 ADD CONSTRAINT ft1_c2negative CHECK (c2 < 0);
EXPLAIN (VERBOSE, COSTS false) SELECT count(*) FROM ft1 WHERE c2 >= 0;
SELECT count(*) FROM ft1 WHERE c2 >= 0;
SET constraint_exclusion = 'on';
EXPLAIN (VERBOSE, COSTS false) SELECT count(*) FROM ft1 WHERE c2 >= 0;
SELECT count(*) FROM ft1 WHERE c2 >= 0;
RESET constraint_exclusion;
-- local check constraint is not actually enforced
INSERT INTO ft1(c1, c2) VALUES(1111, 2);
UPDATE ft1 SET c2 = c2 + 1 WHERE c1 = 1;
ALTER FOREIGN TABLE ft1 DROP CONSTRAINT ft1_c2negative;

-- ===================================================================
-- test serial columns (ie, sequence-based defaults)
-- ===================================================================
create table loc1 (f1 serial, f2 text);
create foreign table rem1 (f1 serial, f2 text)
  server loopback options(table_name 'loc1');
select pg_catalog.setval('rem1_f1_seq', 10, false);
insert into loc1(f2) values('hi');
insert into rem1(f2) values('hi remote');
insert into loc1(f2) values('bye');
insert into rem1(f2) values('bye remote');
select * from loc1;
select * from rem1;

-- ===================================================================
-- test local triggers
-- ===================================================================

-- Trigger functions "borrowed" from triggers regress test.
CREATE FUNCTION trigger_func() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'trigger_func(%) called: action = %, when = %, level = %',
		TG_ARGV[0], TG_OP, TG_WHEN, TG_LEVEL;
	RETURN NULL;
END;$$;

CREATE TRIGGER trig_stmt_before BEFORE DELETE OR INSERT OR UPDATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();
CREATE TRIGGER trig_stmt_after AFTER DELETE OR INSERT OR UPDATE ON rem1
	FOR EACH STATEMENT EXECUTE PROCEDURE trigger_func();

CREATE OR REPLACE FUNCTION trigger_data()  RETURNS trigger
LANGUAGE plpgsql AS $$

declare
	oldnew text[];
	relid text;
    argstr text;
begin

	relid := TG_relid::regclass;
	argstr := '';
	for i in 0 .. TG_nargs - 1 loop
		if i > 0 then
			argstr := argstr || ', ';
		end if;
		argstr := argstr || TG_argv[i];
	end loop;

    RAISE NOTICE '%(%) % % % ON %',
		tg_name, argstr, TG_when, TG_level, TG_OP, relid;
    oldnew := '{}'::text[];
	if TG_OP != 'INSERT' then
		oldnew := array_append(oldnew, format('OLD: %s', OLD));
	end if;

	if TG_OP != 'DELETE' then
		oldnew := array_append(oldnew, format('NEW: %s', NEW));
	end if;

    RAISE NOTICE '%', array_to_string(oldnew, ',');

	if TG_OP = 'DELETE' then
		return OLD;
	else
		return NEW;
	end if;
end;
$$;

-- Test basic functionality
CREATE TRIGGER trig_row_before
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

delete from rem1;
insert into rem1 values(1,'insert');
update rem1 set f2  = 'update' where f1 = 1;
update rem1 set f2 = f2 || f2;


-- cleanup
DROP TRIGGER trig_row_before ON rem1;
DROP TRIGGER trig_row_after ON rem1;
DROP TRIGGER trig_stmt_before ON rem1;
DROP TRIGGER trig_stmt_after ON rem1;

DELETE from rem1;


-- Test WHEN conditions

CREATE TRIGGER trig_row_before_insupd
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW
WHEN (NEW.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after_insupd
AFTER INSERT OR UPDATE ON rem1
FOR EACH ROW
WHEN (NEW.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

-- Insert or update not matching: nothing happens
INSERT INTO rem1 values(1, 'insert');
UPDATE rem1 set f2 = 'test';

-- Insert or update matching: triggers are fired
INSERT INTO rem1 values(2, 'update');
UPDATE rem1 set f2 = 'update update' where f1 = '2';

CREATE TRIGGER trig_row_before_delete
BEFORE DELETE ON rem1
FOR EACH ROW
WHEN (OLD.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after_delete
AFTER DELETE ON rem1
FOR EACH ROW
WHEN (OLD.f2 like '%update%')
EXECUTE PROCEDURE trigger_data(23,'skidoo');

-- Trigger is fired for f1=2, not for f1=1
DELETE FROM rem1;

-- cleanup
DROP TRIGGER trig_row_before_insupd ON rem1;
DROP TRIGGER trig_row_after_insupd ON rem1;
DROP TRIGGER trig_row_before_delete ON rem1;
DROP TRIGGER trig_row_after_delete ON rem1;


-- Test various RETURN statements in BEFORE triggers.

CREATE FUNCTION trig_row_before_insupdate() RETURNS TRIGGER AS $$
  BEGIN
    NEW.f2 := NEW.f2 || ' triggered !';
    RETURN NEW;
  END
$$ language plpgsql;

CREATE TRIGGER trig_row_before_insupd
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

-- The new values should have 'triggered' appended
INSERT INTO rem1 values(1, 'insert');
SELECT * from loc1;
INSERT INTO rem1 values(2, 'insert') RETURNING f2;
SELECT * from loc1;
UPDATE rem1 set f2 = '';
SELECT * from loc1;
UPDATE rem1 set f2 = 'skidoo' RETURNING f2;
SELECT * from loc1;

DELETE FROM rem1;

-- Add a second trigger, to check that the changes are propagated correctly
-- from trigger to trigger
CREATE TRIGGER trig_row_before_insupd2
BEFORE INSERT OR UPDATE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

INSERT INTO rem1 values(1, 'insert');
SELECT * from loc1;
INSERT INTO rem1 values(2, 'insert') RETURNING f2;
SELECT * from loc1;
UPDATE rem1 set f2 = '';
SELECT * from loc1;
UPDATE rem1 set f2 = 'skidoo' RETURNING f2;
SELECT * from loc1;

DROP TRIGGER trig_row_before_insupd ON rem1;
DROP TRIGGER trig_row_before_insupd2 ON rem1;

DELETE from rem1;

INSERT INTO rem1 VALUES (1, 'test');

-- Test with a trigger returning NULL
CREATE FUNCTION trig_null() RETURNS TRIGGER AS $$
  BEGIN
    RETURN NULL;
  END
$$ language plpgsql;

CREATE TRIGGER trig_null
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trig_null();

-- Nothing should have changed.
INSERT INTO rem1 VALUES (2, 'test2');

SELECT * from loc1;

UPDATE rem1 SET f2 = 'test2';

SELECT * from loc1;

DELETE from rem1;

SELECT * from loc1;

DROP TRIGGER trig_null ON rem1;
DELETE from rem1;

-- Test a combination of local and remote triggers
CREATE TRIGGER trig_row_before
BEFORE INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_row_after
AFTER INSERT OR UPDATE OR DELETE ON rem1
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER trig_local_before BEFORE INSERT OR UPDATE ON loc1
FOR EACH ROW EXECUTE PROCEDURE trig_row_before_insupdate();

INSERT INTO rem1(f2) VALUES ('test');
UPDATE rem1 SET f2 = 'testo';

-- Test returning a system attribute
INSERT INTO rem1(f2) VALUES ('test') RETURNING ctid;

-- ===================================================================
-- test inheritance features
-- ===================================================================

CREATE TABLE a (aa TEXT);
CREATE TABLE loct (aa TEXT, bb TEXT);
CREATE FOREIGN TABLE b (bb TEXT) INHERITS (a)
  SERVER loopback OPTIONS (table_name 'loct');

INSERT INTO a(aa) VALUES('aaa');
INSERT INTO a(aa) VALUES('aaaa');
INSERT INTO a(aa) VALUES('aaaaa');

INSERT INTO b(aa) VALUES('bbb');
INSERT INTO b(aa) VALUES('bbbb');
INSERT INTO b(aa) VALUES('bbbbb');

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE a SET aa = 'zzzzzz' WHERE aa LIKE 'aaaa%';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE b SET aa = 'new';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

UPDATE a SET aa = 'newtoo';

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

DELETE FROM a;

SELECT tableoid::regclass, * FROM a;
SELECT tableoid::regclass, * FROM b;
SELECT tableoid::regclass, * FROM ONLY a;

DROP TABLE a CASCADE;
DROP TABLE loct;

-- Check SELECT FOR UPDATE/SHARE with an inherited source table
create table loct1 (f1 int, f2 int, f3 int);
create table loct2 (f1 int, f2 int, f3 int);

create table foo (f1 int, f2 int);
create foreign table foo2 (f3 int) inherits (foo)
  server loopback options (table_name 'loct1');
create table bar (f1 int, f2 int);
create foreign table bar2 (f3 int) inherits (bar)
  server loopback options (table_name 'loct2');

insert into foo values(1,1);
insert into foo values(3,3);
insert into foo2 values(2,2,2);
insert into foo2 values(4,4,4);
insert into bar values(1,11);
insert into bar values(2,22);
insert into bar values(6,66);
insert into bar2 values(3,33,33);
insert into bar2 values(4,44,44);
insert into bar2 values(7,77,77);

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo) for update;
select * from bar where f1 in (select f1 from foo) for update;

explain (verbose, costs off)
select * from bar where f1 in (select f1 from foo) for share;
select * from bar where f1 in (select f1 from foo) for share;

-- Check UPDATE with inherited target and an inherited source table
explain (verbose, costs off)
update bar set f2 = f2 + 100 where f1 in (select f1 from foo);
update bar set f2 = f2 + 100 where f1 in (select f1 from foo);

select tableoid::regclass, * from bar order by 1,2;

-- Check UPDATE with inherited target and an appendrel subquery
explain (verbose, costs off)
update bar set f2 = f2 + 100
from
  ( select f1 from foo union all select f1+3 from foo ) ss
where bar.f1 = ss.f1;
update bar set f2 = f2 + 100
from
  ( select f1 from foo union all select f1+3 from foo ) ss
where bar.f1 = ss.f1;

select tableoid::regclass, * from bar order by 1,2;

-- Test forcing the remote server to produce sorted data for a merge join,
-- but the foreign table is an inheritance child.
truncate table loct1;
truncate table only foo;
\set num_rows_foo 2000
insert into loct1 select generate_series(0, :num_rows_foo, 2), generate_series(0, :num_rows_foo, 2), generate_series(0, :num_rows_foo, 2);
insert into foo select generate_series(1, :num_rows_foo, 2), generate_series(1, :num_rows_foo, 2);
SET enable_hashjoin to false;
SET enable_nestloop to false;
alter foreign table foo2 options (use_remote_estimate 'true');
create index i_loct1_f1 on loct1(f1);
create index i_foo_f1 on foo(f1);
analyze foo;
analyze loct1;
-- inner join; expressions in the clauses appear in the equivalence class list
explain (verbose, costs off)
	select foo.f1, loct1.f1 from foo join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
select foo.f1, loct1.f1 from foo join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
-- outer join; expressions in the clauses do not appear in equivalence class
-- list but no output change as compared to the previous query
explain (verbose, costs off)
	select foo.f1, loct1.f1 from foo left join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
select foo.f1, loct1.f1 from foo left join loct1 on (foo.f1 = loct1.f1) order by foo.f2 offset 10 limit 10;
RESET enable_hashjoin;
RESET enable_nestloop;

-- Test that WHERE CURRENT OF is not supported
begin;
declare c cursor for select * from bar where f1 = 7;
fetch from c;
update bar set f2 = null where current of c;
rollback;

drop table foo cascade;
drop table bar cascade;
drop table loct1;
drop table loct2;

-- ===================================================================
-- test IMPORT FOREIGN SCHEMA
-- ===================================================================

CREATE SCHEMA import_source;
CREATE TABLE import_source.t1 (c1 int, c2 varchar NOT NULL);
CREATE TABLE import_source.t2 (c1 int default 42, c2 varchar NULL, c3 text collate "POSIX");
CREATE TYPE typ1 AS (m1 int, m2 varchar);
CREATE TABLE import_source.t3 (c1 timestamptz default now(), c2 typ1);
CREATE TABLE import_source."x 4" (c1 float8, "C 2" text, c3 varchar(42));
CREATE TABLE import_source."x 5" (c1 float8);
ALTER TABLE import_source."x 5" DROP COLUMN c1;

CREATE SCHEMA import_dest1;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest1;
\det+ import_dest1
\d import_dest1.*

-- Options
CREATE SCHEMA import_dest2;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest2
  OPTIONS (import_default 'true');
\det+ import_dest2
\d import_dest2.*
CREATE SCHEMA import_dest3;
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest3
  OPTIONS (import_collate 'false', import_not_null 'false');
\det+ import_dest3
\d import_dest3.*

-- Check LIMIT TO and EXCEPT
CREATE SCHEMA import_dest4;
IMPORT FOREIGN SCHEMA import_source LIMIT TO (t1, nonesuch)
  FROM SERVER loopback INTO import_dest4;
\det+ import_dest4
IMPORT FOREIGN SCHEMA import_source EXCEPT (t1, "x 4", nonesuch)
  FROM SERVER loopback INTO import_dest4;
\det+ import_dest4

-- Assorted error cases
IMPORT FOREIGN SCHEMA import_source FROM SERVER loopback INTO import_dest4;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER loopback INTO import_dest4;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER loopback INTO notthere;
IMPORT FOREIGN SCHEMA nonesuch FROM SERVER nowhere INTO notthere;

-- Check case of a type present only on the remote server.
-- We can fake this by dropping the type locally in our transaction.
CREATE TYPE "Colors" AS ENUM ('red', 'green', 'blue');
CREATE TABLE import_source.t5 (c1 int, c2 text collate "C", "Col" "Colors");

CREATE SCHEMA import_dest5;
BEGIN;
DROP TYPE "Colors" CASCADE;
IMPORT FOREIGN SCHEMA import_source LIMIT TO (t5)
  FROM SERVER loopback INTO import_dest5;  -- ERROR
ROLLBACK;
