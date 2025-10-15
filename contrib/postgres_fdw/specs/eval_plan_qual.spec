# Tests for the EvalPlanQual mechanism involving foreign tables

setup
{
    DO $d$
        BEGIN
            EXECUTE $$CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
                OPTIONS (dbname '$$||current_database()||$$',
                         port '$$||current_setting('port')||$$'
                )$$;
        END;
    $d$;
    CREATE USER MAPPING FOR PUBLIC SERVER loopback;

    CREATE TABLE a (i int);
    CREATE TABLE b (i int);
    CREATE TABLE c (i int);
    CREATE FOREIGN TABLE fb (i int) SERVER loopback OPTIONS (table_name 'b');
    CREATE FOREIGN TABLE fc (i int) SERVER loopback OPTIONS (table_name 'c');

    INSERT INTO a VALUES (1);
    INSERT INTO b VALUES (1);
    INSERT INTO c VALUES (1);
}

teardown
{
    DROP TABLE a;
    DROP TABLE b;
    DROP TABLE c;
    DROP SERVER loopback CASCADE;
}

session s0
step s0_begin { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s0_update { UPDATE a SET i = i + 1; }
step s0_commit { COMMIT; }

session s1
step s1_begin { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1_tuplock {
    -- Verify if the sub-select has a foreign-join plan
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT a.i,
        (SELECT 1 FROM fb, fc WHERE a.i = fb.i AND fb.i = fc.i)
    FROM a FOR UPDATE;
    SELECT a.i,
        (SELECT 1 FROM fb, fc WHERE a.i = fb.i AND fb.i = fc.i)
    FROM a FOR UPDATE;
}
step s1_commit { COMMIT; }

# This test exercises EvalPlanQual with a SubLink sub-select (which should
# be unaffected by any EPQ recheck behavior in the outer query).
permutation s0_begin s0_update s1_begin s1_tuplock s0_commit s1_commit
