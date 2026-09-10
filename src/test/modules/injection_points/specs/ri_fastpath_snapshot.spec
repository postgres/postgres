# Verify that the per-row RI probe sees a referenced row committed while
# the probe is parked at ri-before-pk-lock.
#
# Lack of SELECT permission on the referenced table makes RI_Initial_Check
# fall back to per-row validation.

setup
{
    CREATE EXTENSION injection_points;
    CREATE ROLE regress_ri_snapshot;
    CREATE TABLE ri_snapshot_pk (id int PRIMARY KEY);
    CREATE TABLE ri_snapshot_fk (pid int);
    INSERT INTO ri_snapshot_fk VALUES (500);
    ALTER TABLE ri_snapshot_fk ADD CONSTRAINT ri_snapshot_fkey
        FOREIGN KEY (pid) REFERENCES ri_snapshot_pk NOT VALID;
    ALTER TABLE ri_snapshot_fk OWNER TO regress_ri_snapshot;
}

teardown
{
    DROP TABLE ri_snapshot_fk, ri_snapshot_pk;
    DROP ROLE regress_ri_snapshot;
    DROP EXTENSION injection_points;
}

session s1
setup
{
    DO $$BEGIN
        PERFORM injection_points_set_local();
        PERFORM injection_points_attach('ri-before-pk-lock', 'wait');
    END$$;
    SET ROLE regress_ri_snapshot;
    SET default_transaction_isolation = 'read committed';
}
step validate { ALTER TABLE ri_snapshot_fk VALIDATE CONSTRAINT ri_snapshot_fkey; }
step checked { }

session s2
step ins { INSERT INTO ri_snapshot_pk VALUES (500); }
step wake
{
    DO $$BEGIN
        PERFORM injection_points_detach('ri-before-pk-lock');
        PERFORM injection_points_wakeup('ri-before-pk-lock');
    END$$;
}
step validated { SELECT convalidated FROM pg_constraint WHERE conname = 'ri_snapshot_fkey'; }

permutation validate ins wake checked validated
