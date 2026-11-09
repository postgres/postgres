# EvalPlanQual rechecks a non-target relation by storing the whole-row datum
# the main plan formed for it (ROW_MARK_COPY) into the EPQ plan's scan slot.
# The EPQ plan is only initialized at the first recheck, so for a function
# returning a composite type, the scan slot's descriptor may be built from a
# later definition of the type than the datum.
#
# The altered column changes from bytea to text, for the reasons explained in
# src/test/regress/sql/rowtypes_alter.sql.

setup
{
    CREATE TYPE rta_type AS (b bytea);
    CREATE FUNCTION rta_func() RETURNS rta_type LANGUAGE plpgsql AS $$
    BEGIN
        RETURN ROW('\x4649584d45'::bytea);
    END;
    $$;
    CREATE FUNCTION rta_exec(cmd text) RETURNS int LANGUAGE plpgsql AS $$
    BEGIN
        EXECUTE cmd;
        RETURN 1;
    END;
    $$;
    CREATE TABLE rta_lockme ();
    CREATE TABLE rta_tgt (s text, n int);
    INSERT INTO rta_tgt VALUES (NULL, NULL);
}

teardown
{
    DROP TABLE rta_tgt, rta_lockme;
    DROP FUNCTION rta_exec(text), rta_func();
    DROP TYPE rta_type;
}

session s1
setup { SET default_transaction_isolation = 'read committed'; }
# The ALTER runs after x::text was computed, but before the recheck.  As an
# uncorrelated subselect it is evaluated once, not again during the recheck.
step s1_upd_alter
{
    UPDATE rta_tgt
    SET s = x::text,
        n = (SELECT rta_exec('ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text'))
    FROM rta_func() x
    RETURNING s;
}
# Same with a column reference.  That fails the recheck's Var type check, but
# only after the datum was deformed with the wrong descriptor.
step s1_upd_alter_col
{
    UPDATE rta_tgt
    SET s = x.b::text,
        n = (SELECT rta_exec('ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text'))
    FROM rta_func() x
    RETURNING s;
}
# Same, but s3 alters.  Acquiring the lock s3 holds processes the
# invalidation before the recheck.
step s1_upd_lock
{
    UPDATE rta_tgt
    SET s = x::text,
        n = (SELECT rta_exec('LOCK rta_lockme'))
    FROM rta_func() x
    RETURNING s;
}

session s2
step s2_upd { BEGIN; UPDATE rta_tgt SET n = 0; }
step s2_commit { COMMIT; }

session s3
step s3_lock { BEGIN; LOCK rta_lockme; }
step s3_alter { ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text; COMMIT; }

# no recheck
permutation s1_upd_alter
# recheck after s1 altered the type
permutation s2_upd s1_upd_alter s2_commit
permutation s2_upd s1_upd_alter_col s2_commit
# recheck after s3 altered the type
permutation s2_upd s3_lock s1_upd_lock s3_alter s2_commit
