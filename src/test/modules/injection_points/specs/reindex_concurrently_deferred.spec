# REINDEX CONCURRENTLY with DEFERRED constraints
#
# Verify that concurrent writes that temporarily violate a deferred unique
# constraint do not fail while REINDEX CONCURRENTLY is running.
#
# The injection point "reindex-conc-index-built" fires after the phase 2
# of REINDEX CONCURRENTLY, when the new index has indisready = true (inserts
# are checked against it) but indisvalid = false.

setup
{
    CREATE EXTENSION injection_points;
    CREATE TABLE reind_deferred (id int, val int,
        CONSTRAINT uq_val UNIQUE(val) DEFERRABLE INITIALLY DEFERRED);
    INSERT INTO reind_deferred VALUES (1, 1), (2, 2);
}

teardown
{
    DROP TABLE reind_deferred;
    DROP EXTENSION injection_points;
}

session s1
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('reindex-conc-index-built', 'wait');
}
step reindex { REINDEX TABLE CONCURRENTLY reind_deferred; }
step noop1 { }

session s2
step check_catalog {
    SELECT c.relname, i.indisunique, i.indimmediate, i.indisready, i.indisvalid
    FROM pg_class c
    JOIN pg_index i ON i.indexrelid = c.oid
    WHERE c.relname = 'uq_val_ccnew';
}
step begin2 { BEGIN; }
step write2 { INSERT INTO reind_deferred VALUES (3, 9); }
step write_dup { INSERT INTO reind_deferred VALUES (4, 9); }
step resolve_dup { UPDATE reind_deferred SET val = 10 WHERE id = 4; }
step commit2 { COMMIT; }
step wakeup {
    SELECT injection_points_detach('reindex-conc-index-built');
    SELECT injection_points_wakeup('reindex-conc-index-built');
}

permutation reindex check_catalog begin2 write2 write_dup resolve_dup commit2 wakeup noop1
