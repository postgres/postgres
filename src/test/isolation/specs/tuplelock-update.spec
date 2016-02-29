setup {
   DROP TABLE IF EXISTS pktab;
   CREATE TABLE pktab (id int PRIMARY KEY, data SERIAL NOT NULL);
   INSERT INTO pktab VALUES (1, DEFAULT);
}

teardown {
   DROP TABLE pktab;
}

session "s1"
step "s1_advlock" {
    SELECT pg_advisory_lock(142857),
        pg_advisory_lock(285714),
        pg_advisory_lock(571428);
 }
step "s1_chain" { UPDATE pktab SET data = DEFAULT; }
step "s1_begin" { BEGIN; }
step "s1_grablock" { SELECT * FROM pktab FOR KEY SHARE; }
step "s1_advunlock1" { SELECT pg_advisory_unlock(142857); }
step "s1_advunlock2" { SELECT pg_sleep(5), pg_advisory_unlock(285714); }
step "s1_advunlock3" { SELECT pg_sleep(5), pg_advisory_unlock(571428); }
step "s1_commit" { COMMIT; }

session "s2"
step "s2_update" { UPDATE pktab SET data = DEFAULT WHERE pg_advisory_lock_shared(142857) IS NOT NULL; }

session "s3"
step "s3_update" { UPDATE pktab SET data = DEFAULT WHERE pg_advisory_lock_shared(285714) IS NOT NULL; }

session "s4"
step "s4_update" { UPDATE pktab SET data = DEFAULT WHERE pg_advisory_lock_shared(571428) IS NOT NULL; }

permutation "s1_advlock" "s2_update" "s3_update" "s4_update" "s1_chain" "s1_begin" "s1_grablock" "s1_advunlock1" "s1_advunlock2" "s1_advunlock3" "s1_commit"
