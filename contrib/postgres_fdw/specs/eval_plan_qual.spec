# Tests for the EvalPlanQual mechanism involving foreign tables

setup
{
    DO $d$
        BEGIN
            EXECUTE $$CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
                OPTIONS (dbname '$$||current_database()||$$',
                         port '$$||current_setting('port')||$$',
                         use_remote_estimate 'true'
                )$$;
        END;
    $d$;
    CREATE USER MAPPING FOR PUBLIC SERVER loopback;

    CREATE TABLE l (i int, v text);
    CREATE TABLE t (i int, v text);
    CREATE FOREIGN TABLE ft (i int, v text) SERVER loopback OPTIONS (table_name 't');

    INSERT INTO l VALUES (123, 'foo'), (456, 'bar'), (789, 'baz');
    INSERT INTO t SELECT i, to_char(i, 'FM0000') FROM generate_series(1, 1000) i;
    CREATE INDEX t_idx ON t (i);
    ANALYZE l, t;

    CREATE TABLE a (i int);
    CREATE TABLE b (i int);
    CREATE TABLE c (i int);
    CREATE FOREIGN TABLE fb (i int) SERVER loopback OPTIONS (table_name 'b');
    CREATE FOREIGN TABLE fc (i int) SERVER loopback OPTIONS (table_name 'c');

    INSERT INTO a VALUES (1);
    INSERT INTO b VALUES (1);
    INSERT INTO c VALUES (1);
    ANALYZE a, b, c;
}

teardown
{
    DROP TABLE l;
    DROP TABLE t;
    DROP TABLE a;
    DROP TABLE b;
    DROP TABLE c;
    DROP SERVER loopback CASCADE;
}

session s0
setup { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s0_update_l { UPDATE l SET i = i + 1; }
step s0_update_a { UPDATE a SET i = i + 1; }
step s0_commit { COMMIT; }

session s1
setup { BEGIN ISOLATION LEVEL READ COMMITTED; }

# Test for EPQ with a foreign scan pushing down a qual
step s1_tuplock_l_0 {
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT l.* FROM l, ft WHERE l.i = ft.i AND l.i = 123 FOR UPDATE OF l;
    SELECT l.* FROM l, ft WHERE l.i = ft.i AND l.i = 123 FOR UPDATE OF l;
}

# Same test, except that the qual is parameterized
step s1_tuplock_l_1 {
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT l.* FROM l, ft WHERE l.i = ft.i AND l.v = 'foo' FOR UPDATE OF l;
    SELECT l.* FROM l, ft WHERE l.i = ft.i AND l.v = 'foo' FOR UPDATE OF l;
}

# Test for EPQ with a foreign scan pushing down a join
step s1_tuplock_a_0 {
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT a.i FROM a, fb, fc WHERE a.i = fb.i AND fb.i = fc.i FOR UPDATE OF a;
    SELECT a.i FROM a, fb, fc WHERE a.i = fb.i AND fb.i = fc.i FOR UPDATE OF a;
}

# Same test, except that the join is contained in a SubLink sub-select, not
# in the main query
step s1_tuplock_a_1 {
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT a.i,
        (SELECT 1 FROM fb, fc WHERE a.i = fb.i AND fb.i = fc.i)
    FROM a FOR UPDATE;
    SELECT a.i,
        (SELECT 1 FROM fb, fc WHERE a.i = fb.i AND fb.i = fc.i)
    FROM a FOR UPDATE;
}

step s1_commit { COMMIT; }

# This test checks the case of rechecking a pushed-down qual.
permutation s0_update_l s1_tuplock_l_0 s0_commit s1_commit

# This test checks the same case, except that the qual is parameterized.
permutation s0_update_l s1_tuplock_l_1 s0_commit s1_commit

# This test checks the case of rechecking a pushed-down join.
permutation s0_update_a s1_tuplock_a_0 s0_commit s1_commit

# This test exercises EvalPlanQual with a SubLink sub-select (which should
# be unaffected by any EPQ recheck behavior in the outer query).
permutation s0_update_a s1_tuplock_a_1 s0_commit s1_commit
