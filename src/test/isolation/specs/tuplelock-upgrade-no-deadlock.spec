# This test checks that multiple sessions locking a single row in a table
# do not deadlock each other when one of them upgrades its existing lock
# while the others are waiting for it.

setup
{
    drop table if exists tlu_job;
    create table tlu_job (id integer primary key, name text);

    insert into tlu_job values(1, 'a');
}


teardown
{
    drop table tlu_job;
}

session "s0"
step "s0_begin" { begin; }
step "s0_keyshare" { select id from tlu_job where id = 1 for key share;}
step "s0_rollback" { rollback; }

session "s1"
setup { begin; }
step "s1_keyshare" { select id from tlu_job where id = 1 for key share;}
step "s1_share" { select id from tlu_job where id = 1 for share; }
step "s1_fornokeyupd" { select id from tlu_job where id = 1 for no key update; }
step "s1_update" { update tlu_job set name = 'b' where id = 1;  }
step "s1_savept_e" { savepoint s1_e; }
step "s1_savept_f" { savepoint s1_f; }
step "s1_rollback_e" { rollback to s1_e; }
step "s1_rollback_f" { rollback to s1_f; }
step "s1_rollback" { rollback; }
step "s1_commit" { commit; }

session "s2"
setup { begin; }
step "s2_for_keyshare" { select id from tlu_job where id = 1 for key share; }
step "s2_fornokeyupd" { select id from tlu_job where id = 1 for no key update; }
step "s2_for_update" { select id from tlu_job where id = 1 for update; }
step "s2_update" { update tlu_job set name = 'b' where id = 1; }
step "s2_delete" { delete from tlu_job where id = 1; }
step "s2_rollback" { rollback; }

session "s3"
setup { begin; }
step "s3_keyshare" { select id from tlu_job where id = 1 for key share; }
step "s3_share" { select id from tlu_job where id = 1 for share; }
step "s3_for_update" { select id from tlu_job where id = 1 for update; }
step "s3_update" { update tlu_job set name = 'c' where id = 1; }
step "s3_delete" { delete from tlu_job where id = 1; }
step "s3_rollback" { rollback; }
step "s3_commit" { commit; }

# test that s2 will not deadlock with s3 when s1 is rolled back
permutation "s1_share" "s2_for_update" "s3_share" "s3_for_update" "s1_rollback" "s3_rollback" "s2_rollback"
# test that update does not cause deadlocks if it can proceed
permutation "s1_keyshare" "s2_for_update" "s3_keyshare" "s1_update" "s3_update" "s1_rollback" "s3_rollback" "s2_rollback"
permutation "s1_keyshare" "s2_for_update" "s3_keyshare" "s1_update" "s3_update" "s1_commit"   "s3_rollback" "s2_rollback"
# test that delete does not cause deadlocks if it can proceed
permutation "s1_keyshare" "s2_for_update" "s3_keyshare"  "s3_delete" "s1_rollback" "s3_rollback" "s2_rollback"
permutation "s1_keyshare" "s2_for_update" "s3_keyshare"  "s3_delete" "s1_rollback" "s3_commit"   "s2_rollback"
# test that sessions that don't upgrade locks acquire them in order
permutation "s1_share" "s2_for_update" "s3_for_update" "s1_rollback" "s2_rollback" "s3_rollback"
permutation "s1_share" "s2_update" "s3_update" "s1_rollback" "s2_rollback" "s3_rollback"
permutation "s1_share" "s2_delete" "s3_delete" "s1_rollback" "s2_rollback" "s3_rollback"
# test s2 retrying the overall tuple lock algorithm after initially avoiding deadlock
permutation "s1_keyshare" "s3_for_update" "s2_for_keyshare" "s1_savept_e" "s1_share" "s1_savept_f" "s1_fornokeyupd" "s2_fornokeyupd" "s0_begin" "s0_keyshare" "s1_rollback_f" "s0_keyshare" "s1_rollback_e" "s1_rollback" "s2_rollback" "s0_rollback" "s3_rollback"
