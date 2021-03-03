# Test decoding of two-phase transactions during the build of a consistent snapshot.
setup
{
    DROP TABLE IF EXISTS do_write;
    CREATE TABLE do_write(id serial primary key);
}

teardown
{
    DROP TABLE do_write;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}


session "s1"
setup { SET synchronous_commit=on; }

step "s1init" {SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding', false, true);}
step "s1start" {SELECT data  FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', 'false', 'skip-empty-xacts', '1');}
step "s1insert" { INSERT INTO do_write DEFAULT VALUES; }

session "s2"
setup { SET synchronous_commit=on; }

step "s2b" { BEGIN; }
step "s2txid" { SELECT pg_current_xact_id() IS NULL; }
step "s2c" { COMMIT; }
step "s2insert" { INSERT INTO do_write DEFAULT VALUES; }
step "s2p" { PREPARE TRANSACTION 'test1'; }
step "s2cp" { COMMIT PREPARED 'test1'; }


session "s3"
setup { SET synchronous_commit=on; }

step "s3b" { BEGIN; }
step "s3txid" { SELECT pg_current_xact_id() IS NULL; }
step "s3c" { COMMIT; }

# Force building of a consistent snapshot between a PREPARE and COMMIT PREPARED
# and ensure that the whole transaction is decoded at the time of COMMIT
# PREPARED.
#
# 's1init' step will initialize the replication slot and cause logical decoding
# to wait in initial starting point till the in-progress transaction in s2 is
# committed. 's2c' step will cause logical decoding to go to initial consistent
# point and wait for in-progress transaction s3 to commit. 's3c' step will cause
# logical decoding to find a consistent point while the transaction s2 is
# prepared and not yet committed. This will cause the first s1start to skip
# prepared transaction s2 as that will be before consistent point. The second
# s1start will allow decoding of skipped prepare along with commit prepared done
# as part of s2cp.
permutation "s2b" "s2txid" "s1init" "s3b" "s3txid" "s2c" "s2b" "s2insert" "s2p" "s3c" "s1insert" "s1start" "s2cp" "s1start"
