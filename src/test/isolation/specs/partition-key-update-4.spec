# Test that a row that ends up in a new partition contains changes made by
# a concurrent transaction.

setup
{
  --
  -- Setup to test concurrent handling of ExecDelete().
  --
  CREATE TABLE foo (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE foo1 PARTITION OF foo FOR VALUES IN (1);
  CREATE TABLE foo2 PARTITION OF foo FOR VALUES IN (2);
  INSERT INTO foo VALUES (1, 'ABC');

  --
  -- Setup to test concurrent handling of GetTupleForTrigger().
  --
  CREATE TABLE footrg (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE triglog as select * from footrg;
  CREATE TABLE footrg1 PARTITION OF footrg FOR VALUES IN (1);
  CREATE TABLE footrg2 PARTITION OF footrg FOR VALUES IN (2);
  INSERT INTO footrg VALUES (1, 'ABC');
  CREATE FUNCTION func_footrg() RETURNS TRIGGER AS $$
  BEGIN
	 OLD.b = OLD.b || ' trigger';

	 -- This will verify that the trigger is not run *before* the row is
	 -- refetched by EvalPlanQual. The OLD row should contain the changes made
	 -- by the concurrent session.
     INSERT INTO triglog select OLD.*;

     RETURN OLD;
  END $$ LANGUAGE PLPGSQL;
  CREATE TRIGGER footrg_ondel BEFORE DELETE ON footrg1
   FOR EACH ROW EXECUTE PROCEDURE func_footrg();

}

teardown
{
  DROP TABLE foo;
  DROP TRIGGER footrg_ondel ON footrg1;
  DROP FUNCTION func_footrg();
  DROP TABLE footrg;
  DROP TABLE triglog;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE foo SET a = a + 1, b = b || ' update1' WHERE b like '%ABC%'; }
step s1ut	{ UPDATE footrg SET a = a + 1, b = b || ' update1' WHERE b like '%ABC%'; }
step s1s	{ SELECT tableoid::regclass, * FROM foo ORDER BY a; }
step s1st	{ SELECT tableoid::regclass, * FROM footrg ORDER BY a; }
step s1stl	{ SELECT * FROM triglog ORDER BY a; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2u1	{ UPDATE foo SET b = b || ' update2' WHERE a = 1; }
step s2u2	{ UPDATE foo SET b = 'EFG' WHERE a = 1; }
step s2ut1	{ UPDATE footrg SET b = b || ' update2' WHERE a = 1; }
step s2ut2	{ UPDATE footrg SET b = 'EFG' WHERE a = 1; }
step s2c	{ COMMIT; }


# Session s1 is moving a row into another partition, but is waiting for
# another session s2 that is updating the original row. The row that ends up
# in the new partition should contain the changes made by session s2.
permutation s1b s2b s2u1 s1u s2c s1c s1s

# Same as above, except, session s1 is waiting in GetTupleForTrigger().
permutation s1b s2b s2ut1 s1ut s2c s1c s1st s1stl

# Below two cases are similar to the above two; except that the session s1
# fails EvalPlanQual() test, so partition key update does not happen.
permutation s1b s2b s2u2 s1u s2c s1c s1s
permutation s1b s2b s2ut2 s1ut s2c s1c s1st s1stl
