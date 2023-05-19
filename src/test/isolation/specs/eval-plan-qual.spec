# Tests for the EvalPlanQual mechanism
#
# EvalPlanQual is used in READ COMMITTED isolation level to attempt to
# re-execute UPDATE and DELETE operations against rows that were updated
# by some concurrent transaction.

setup
{
 CREATE TABLE accounts (accountid text PRIMARY KEY, balance numeric not null,
   balance2 numeric GENERATED ALWAYS AS (balance * 2) STORED);
 INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);

 CREATE FUNCTION update_checking(int) RETURNS bool LANGUAGE sql AS $$
     UPDATE accounts SET balance = balance + 1 WHERE accountid = 'checking'; SELECT true;$$;

 CREATE TABLE accounts_ext (accountid text PRIMARY KEY, balance numeric not null, other text);
 INSERT INTO accounts_ext VALUES ('checking', 600, 'other'), ('savings', 700, null);
 ALTER TABLE accounts_ext ADD COLUMN newcol int DEFAULT 42;
 ALTER TABLE accounts_ext ADD COLUMN newcol2 text DEFAULT NULL;

 CREATE TABLE p (a int, b int, c int);
 CREATE TABLE c1 () INHERITS (p);
 CREATE TABLE c2 () INHERITS (p);
 CREATE TABLE c3 () INHERITS (p);
 INSERT INTO c1 SELECT 0, a / 3, a % 3 FROM generate_series(0, 9) a;
 INSERT INTO c2 SELECT 1, a / 3, a % 3 FROM generate_series(0, 9) a;
 INSERT INTO c3 SELECT 2, a / 3, a % 3 FROM generate_series(0, 9) a;

 CREATE TABLE table_a (id integer, value text);
 CREATE TABLE table_b (id integer, value text);
 INSERT INTO table_a VALUES (1, 'tableAValue');
 INSERT INTO table_b VALUES (1, 'tableBValue');

 CREATE TABLE jointest AS SELECT generate_series(1,10) AS id, 0 AS data;
 CREATE INDEX ON jointest(id);

 CREATE TABLE parttbl (a int, b int, c int,
   d int GENERATED ALWAYS AS (a + b) STORED) PARTITION BY LIST (a);
 CREATE TABLE parttbl1 PARTITION OF parttbl FOR VALUES IN (1);
 CREATE TABLE parttbl2 PARTITION OF parttbl FOR VALUES IN (2);
 INSERT INTO parttbl VALUES (1, 1, 1), (2, 2, 2);

 CREATE TABLE another_parttbl (a int, b int, c int) PARTITION BY LIST (a);
 CREATE TABLE another_parttbl1 PARTITION OF another_parttbl FOR VALUES IN (1);
 CREATE TABLE another_parttbl2 PARTITION OF another_parttbl FOR VALUES IN (2);
 INSERT INTO another_parttbl VALUES (1, 1, 1);

 CREATE FUNCTION noisy_oper(p_comment text, p_a anynonarray, p_op text, p_b anynonarray)
 RETURNS bool LANGUAGE plpgsql AS $$
 DECLARE
  r bool;
  BEGIN
  EXECUTE format('SELECT $1 %s $2', p_op) INTO r USING p_a, p_b;
  RAISE NOTICE '%: % % % % %: %', p_comment, pg_typeof(p_a), p_a, p_op, pg_typeof(p_b), p_b, r;
  RETURN r;
  END;$$;
}

teardown
{
 DROP TABLE accounts;
 DROP FUNCTION update_checking(int);
 DROP TABLE accounts_ext;
 DROP TABLE p CASCADE;
 DROP TABLE table_a, table_b, jointest;
 DROP TABLE parttbl;
 DROP TABLE another_parttbl;
 DROP FUNCTION noisy_oper(text, anynonarray, text, anynonarray)
}

session s1
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
# wx1 then wx2 checks the basic case of re-fetching up-to-date values
step wx1	{ UPDATE accounts SET balance = balance - 200 WHERE accountid = 'checking' RETURNING balance; }
# wy1 then wy2 checks the case where quals pass then fail
step wy1	{ UPDATE accounts SET balance = balance + 500 WHERE accountid = 'checking' RETURNING balance; }

step wxext1	{ UPDATE accounts_ext SET balance = balance - 200 WHERE accountid = 'checking' RETURNING balance; }
step tocds1	{ UPDATE accounts SET accountid = 'cds' WHERE accountid = 'checking'; }
step tocdsext1 { UPDATE accounts_ext SET accountid = 'cds' WHERE accountid = 'checking'; }

# d1 then wx1 checks that update can deal with the updated row vanishing
# wx2 then d1 checks that the delete affects the updated row
# wx2, wx2 then d1 checks that the delete checks the quals correctly (balance too high)
# wx2, d2, then d1 checks that delete handles a vanishing row correctly
step d1		{ DELETE FROM accounts WHERE accountid = 'checking' AND balance < 1500 RETURNING balance; }

# upsert tests are to check writable-CTE cases
step upsert1	{
	WITH upsert AS
	  (UPDATE accounts SET balance = balance + 500
	   WHERE accountid = 'savings'
	   RETURNING accountid)
	INSERT INTO accounts SELECT 'savings', 500
	  WHERE NOT EXISTS (SELECT 1 FROM upsert);
}

# tests with table p check inheritance cases:
# readp1/writep1/readp2 tests a bug where nodeLockRows did the wrong thing
# when the first updated tuple was in a non-first child table.
# writep2/returningp1 tests a memory allocation issue
# writep3a/writep3b tests updates touching more than one table
# writep4a/writep4b tests a case where matches in another table confused EPQ
# writep4a/deletep4 tests the same case in the DELETE path

step readp		{ SELECT tableoid::regclass, ctid, * FROM p; }
step readp1		{ SELECT tableoid::regclass, ctid, * FROM p WHERE b IN (0, 1) AND c = 0 FOR UPDATE; }
step writep1	{ UPDATE p SET b = -1 WHERE a = 1 AND b = 1 AND c = 0; }
step writep2	{ UPDATE p SET b = -b WHERE a = 1 AND c = 0; }
step writep3a	{ UPDATE p SET b = -b WHERE c = 0; }
step writep4a	{ UPDATE p SET c = 4 WHERE c = 0; }
step c1		{ COMMIT; }
step r1		{ ROLLBACK; }

# these tests are meant to exercise EvalPlanQualFetchRowMark,
# ie, handling non-locked tables in an EvalPlanQual recheck

step partiallock	{
	SELECT * FROM accounts a1, accounts a2
	  WHERE a1.accountid = a2.accountid
	  FOR UPDATE OF a1;
}
step lockwithvalues	{
	-- Reference rowmark column that differs in type from targetlist at some attno.
	-- See CAHU7rYZo_C4ULsAx_LAj8az9zqgrD8WDd4hTegDTMM1LMqrBsg@mail.gmail.com
	SELECT a1.*, v.id FROM accounts a1, (values('checking'::text, 'nan'::text),('savings', 'nan')) v(id, notnumeric)
	WHERE a1.accountid = v.id AND v.notnumeric != 'einszwei'
	  FOR UPDATE OF a1;
}
step partiallock_ext	{
	SELECT * FROM accounts_ext a1, accounts_ext a2
	  WHERE a1.accountid = a2.accountid
	  FOR UPDATE OF a1;
}

# these tests exercise EvalPlanQual with a SubLink sub-select (which should be
# unaffected by any EPQ recheck behavior in the outer query); cf bug #14034

step updateforss	{
	UPDATE table_a SET value = 'newTableAValue' WHERE id = 1;
	UPDATE table_b SET value = 'newTableBValue' WHERE id = 1;
}

# these tests exercise EvalPlanQual with conditional InitPlans which
# have not been executed prior to the EPQ

step updateforcip	{
	UPDATE table_a SET value = NULL WHERE id = 1;
}

# these tests exercise mark/restore during EPQ recheck, cf bug #15032

step selectjoinforupdate	{
	set local enable_nestloop to 0;
	set local enable_hashjoin to 0;
	set local enable_seqscan to 0;
	explain (costs off)
	select * from jointest a join jointest b on a.id=b.id for update;
	select * from jointest a join jointest b on a.id=b.id for update;
}

# these tests exercise Result plan nodes participating in EPQ

step selectresultforupdate	{
	select * from (select 1 as x) ss1 join (select 7 as y) ss2 on true
	  left join table_a a on a.id = x, jointest jt
	  where jt.id = y;
	explain (verbose, costs off)
	select * from (select 1 as x) ss1 join (select 7 as y) ss2 on true
	  left join table_a a on a.id = x, jointest jt
	  where jt.id = y for update of jt, ss1, ss2;
	select * from (select 1 as x) ss1 join (select 7 as y) ss2 on true
	  left join table_a a on a.id = x, jointest jt
	  where jt.id = y for update of jt, ss1, ss2;
}

# test for EPQ on a partitioned result table

step simplepartupdate	{
	update parttbl set b = b + 10;
}

# test scenarios where update may cause row movement

step simplepartupdate_route1to2 {
	update parttbl set a = 2 where c = 1 returning *;
}

step simplepartupdate_noroute {
	update parttbl set b = 2 where c = 1 returning *;
}


session s2
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step wx2	{ UPDATE accounts SET balance = balance + 450 WHERE accountid = 'checking' RETURNING balance; }
step wy2	{ UPDATE accounts SET balance = balance + 1000 WHERE accountid = 'checking' AND balance < 1000  RETURNING balance; }
step d2		{ DELETE FROM accounts WHERE accountid = 'checking'; }

step upsert2	{
	WITH upsert AS
	  (UPDATE accounts SET balance = balance + 1234
	   WHERE accountid = 'savings'
	   RETURNING accountid)
	INSERT INTO accounts SELECT 'savings', 1234
	  WHERE NOT EXISTS (SELECT 1 FROM upsert);
}
step wx2_ext	{ UPDATE accounts_ext SET balance = balance + 450; }
step readp2		{ SELECT tableoid::regclass, ctid, * FROM p WHERE b IN (0, 1) AND c = 0 FOR UPDATE; }
step returningp1 {
	WITH u AS ( UPDATE p SET b = b WHERE a > 0 RETURNING * )
	  SELECT * FROM u;
}
step writep3b	{ UPDATE p SET b = -b WHERE c = 0; }
step writep4b	{ UPDATE p SET b = -4 WHERE c = 0; }
step deletep4	{ DELETE FROM p WHERE c = 0; }
step readforss	{
	SELECT ta.id AS ta_id, ta.value AS ta_value,
		(SELECT ROW(tb.id, tb.value)
		 FROM table_b tb WHERE ta.id = tb.id) AS tb_row
	FROM table_a ta
	WHERE ta.id = 1 FOR UPDATE OF ta;
}
step updateforcip2	{
	UPDATE table_a SET value = COALESCE(value, (SELECT text 'newValue')) WHERE id = 1;
}
step updateforcip3	{
	WITH d(val) AS (SELECT text 'newValue' FROM generate_series(1,1))
	UPDATE table_a SET value = COALESCE(value, (SELECT val FROM d)) WHERE id = 1;
}
step wrtwcte	{ UPDATE table_a SET value = 'tableAValue2' WHERE id = 1; }
step wrjt	{ UPDATE jointest SET data = 42 WHERE id = 7; }

step conditionalpartupdate	{
	update parttbl set c = -c where b < 10;
}

step complexpartupdate	{
	with u as (update parttbl set b = b + 1 returning parttbl.*)
	update parttbl p set b = u.b + 100 from u where p.a = u.a;
}

step complexpartupdate_route_err1 {
	with u as (update another_parttbl set a = 1 returning another_parttbl.*)
	update parttbl p set a = u.a from u where p.a = u.a and p.c = 1 returning p.*;
}

step complexpartupdate_route {
	with u as (update another_parttbl set a = 1 returning another_parttbl.*)
	update parttbl p set a = p.b from u where p.a = u.a and p.c = 1 returning p.*;
}

step complexpartupdate_doesnt_route {
	with u as (update another_parttbl set a = 1 returning another_parttbl.*)
	update parttbl p set a = 3 - p.b from u where p.a = u.a and p.c = 1 returning p.*;
}

# Use writable CTEs to create self-updated rows, that then are
# (updated|deleted). The *fail versions of the tests additionally
# perform an update, via a function, in a different command, to test
# behaviour relating to that.
step updwcte  { WITH doup AS (UPDATE accounts SET balance = balance + 1100 WHERE accountid = 'checking' RETURNING *) UPDATE accounts a SET balance = doup.balance + 100 FROM doup RETURNING *; }
step updwctefail  { WITH doup AS (UPDATE accounts SET balance = balance + 1100 WHERE accountid = 'checking' RETURNING *, update_checking(999)) UPDATE accounts a SET balance = doup.balance + 100 FROM doup RETURNING *; }
step delwcte  { WITH doup AS (UPDATE accounts SET balance = balance + 1100 WHERE accountid = 'checking' RETURNING *) DELETE FROM accounts a USING doup RETURNING *; }
step delwctefail  { WITH doup AS (UPDATE accounts SET balance = balance + 1100 WHERE accountid = 'checking' RETURNING *, update_checking(999)) DELETE FROM accounts a USING doup RETURNING *; }

# Check that nested EPQ works correctly
step wnested2 {
    UPDATE accounts SET balance = balance - 1200
    WHERE noisy_oper('upid', accountid, '=', 'checking')
    AND noisy_oper('up', balance, '>', 200.0)
    AND EXISTS (
        SELECT accountid
        FROM accounts_ext ae
        WHERE noisy_oper('lock_id', ae.accountid, '=', accounts.accountid)
            AND noisy_oper('lock_bal', ae.balance, '>', 200.0)
        FOR UPDATE
    );
}

step c2	{ COMMIT; }
step r2	{ ROLLBACK; }

session s3
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step read	{ SELECT * FROM accounts ORDER BY accountid; }
step read_ext	{ SELECT * FROM accounts_ext ORDER BY accountid; }
step read_a		{ SELECT * FROM table_a ORDER BY id; }
step read_part	{ SELECT * FROM parttbl ORDER BY a, c; }

# this test exercises EvalPlanQual with a CTE, cf bug #14328
step readwcte	{
	WITH
	    cte1 AS (
	      SELECT id FROM table_b WHERE value = 'tableBValue'
	    ),
	    cte2 AS (
	      SELECT * FROM table_a
	      WHERE id = (SELECT id FROM cte1)
	      FOR UPDATE
	    )
	SELECT * FROM cte2;
}

# this test exercises a different CTE misbehavior, cf bug #14870
step multireadwcte	{
	WITH updated AS (
	  UPDATE table_a SET value = 'tableAValue3' WHERE id = 1 RETURNING id
	)
	SELECT (SELECT id FROM updated) AS subid, * FROM updated;
}

teardown	{ COMMIT; }

# test that normal update follows update chains, and reverifies quals
permutation wx1 wx2 c1 c2 read
permutation wy1 wy2 c1 c2 read
permutation wx1 wx2 r1 c2 read
permutation wy1 wy2 r1 c2 read

# test that deletes follow chains, and if necessary reverifies quals
permutation wx1 d1 wx2 c1 c2 read
permutation wx2 d1 c2 c1 read
permutation wx2 wx2 d1 c2 c1 read
permutation wx2 d2 d1 c2 c1 read
permutation wx1 d1 wx2 r1 c2 read
permutation wx2 d1 r2 c1 read
permutation wx2 wx2 d1 r2 c1 read
permutation wx2 d2 d1 r2 c1 read
permutation d1 wx2 c1 c2 read
permutation d1 wx2 r1 c2 read

# Check that nested EPQ works correctly
permutation wnested2 c1 c2 read
permutation wx1 wxext1 wnested2 c1 c2 read
permutation wx1 wx1 wxext1 wnested2 c1 c2 read
permutation wx1 wx1 wxext1 wxext1 wnested2 c1 c2 read
permutation wx1 wxext1 wxext1 wnested2 c1 c2 read
permutation wx1 tocds1 wnested2 c1 c2 read
permutation wx1 tocdsext1 wnested2 c1 c2 read

# test that an update to a self-modified row is ignored when
# previously updated by the same cid
permutation wx1 updwcte c1 c2 read
# test that an update to a self-modified row throws error when
# previously updated by a different cid
permutation wx1 updwctefail c1 c2 read
# test that a delete to a self-modified row is ignored when
# previously updated by the same cid
permutation wx1 delwcte c1 c2 read
# test that a delete to a self-modified row throws error when
# previously updated by a different cid
permutation wx1 delwctefail c1 c2 read

permutation upsert1 upsert2 c1 c2 read
permutation readp1 writep1 readp2 c1 c2
permutation writep2 returningp1 c1 c2
permutation writep3a writep3b c1 c2
permutation writep4a writep4b c1 c2 readp
permutation writep4a deletep4 c1 c2 readp
permutation wx2 partiallock c2 c1 read
permutation wx2 lockwithvalues c2 c1 read
permutation wx2_ext partiallock_ext c2 c1 read_ext
permutation updateforss readforss c1 c2
permutation updateforcip updateforcip2 c1 c2 read_a
permutation updateforcip updateforcip3 c1 c2 read_a
permutation wrtwcte readwcte c1 c2
permutation wrjt selectjoinforupdate c2 c1
permutation wrjt selectresultforupdate c2 c1
permutation wrtwcte multireadwcte c1 c2

permutation simplepartupdate conditionalpartupdate c1 c2 read_part
permutation simplepartupdate complexpartupdate c1 c2 read_part
permutation simplepartupdate_route1to2 complexpartupdate_route_err1 c1 c2 read_part
permutation simplepartupdate_noroute complexpartupdate_route c1 c2 read_part
permutation simplepartupdate_noroute complexpartupdate_doesnt_route c1 c2 read_part
