# Here we verify that tuple lock levels conform to their documented
# conflict tables.

setup {
	DROP TABLE IF EXISTS multixact_conflict;
	CREATE TABLE multixact_conflict (a int primary key);
	INSERT INTO multixact_conflict VALUES (1);
}

teardown {
	DROP TABLE multixact_conflict;
}

session "s1"
step "s1_begin" { BEGIN; }
step "s1_lcksvpt" { SELECT * FROM multixact_conflict FOR KEY SHARE; SAVEPOINT foo; }
step "s1_tuplock1" { SELECT * FROM multixact_conflict FOR KEY SHARE; }
step "s1_tuplock2" { SELECT * FROM multixact_conflict FOR SHARE; }
step "s1_tuplock3" { SELECT * FROM multixact_conflict FOR NO KEY UPDATE; }
step "s1_tuplock4" { SELECT * FROM multixact_conflict FOR UPDATE; }
step "s1_commit" { COMMIT; }

session "s2"
step "s2_tuplock1" { SELECT * FROM multixact_conflict FOR KEY SHARE; }
step "s2_tuplock2" { SELECT * FROM multixact_conflict FOR SHARE; }
step "s2_tuplock3" { SELECT * FROM multixact_conflict FOR NO KEY UPDATE; }
step "s2_tuplock4" { SELECT * FROM multixact_conflict FOR UPDATE; }

# The version with savepoints test the multixact cases
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock1" "s2_tuplock1" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock1" "s2_tuplock2" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock1" "s2_tuplock3" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock1" "s2_tuplock4" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock2" "s2_tuplock1" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock2" "s2_tuplock2" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock2" "s2_tuplock3" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock2" "s2_tuplock4" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock3" "s2_tuplock1" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock3" "s2_tuplock2" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock3" "s2_tuplock3" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock3" "s2_tuplock4" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock4" "s2_tuplock1" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock4" "s2_tuplock2" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock4" "s2_tuplock3" "s1_commit"
permutation "s1_begin" "s1_lcksvpt" "s1_tuplock4" "s2_tuplock4" "s1_commit"

# no multixacts here
permutation "s1_begin"              "s1_tuplock1" "s2_tuplock1" "s1_commit"
permutation "s1_begin"              "s1_tuplock1" "s2_tuplock2" "s1_commit"
permutation "s1_begin"              "s1_tuplock1" "s2_tuplock3" "s1_commit"
permutation "s1_begin"              "s1_tuplock1" "s2_tuplock4" "s1_commit"
permutation "s1_begin"              "s1_tuplock2" "s2_tuplock1" "s1_commit"
permutation "s1_begin"              "s1_tuplock2" "s2_tuplock2" "s1_commit"
permutation "s1_begin"              "s1_tuplock2" "s2_tuplock3" "s1_commit"
permutation "s1_begin"              "s1_tuplock2" "s2_tuplock4" "s1_commit"
permutation "s1_begin"              "s1_tuplock3" "s2_tuplock1" "s1_commit"
permutation "s1_begin"              "s1_tuplock3" "s2_tuplock2" "s1_commit"
permutation "s1_begin"              "s1_tuplock3" "s2_tuplock3" "s1_commit"
permutation "s1_begin"              "s1_tuplock3" "s2_tuplock4" "s1_commit"
permutation "s1_begin"              "s1_tuplock4" "s2_tuplock1" "s1_commit"
permutation "s1_begin"              "s1_tuplock4" "s2_tuplock2" "s1_commit"
permutation "s1_begin"              "s1_tuplock4" "s2_tuplock3" "s1_commit"
permutation "s1_begin"              "s1_tuplock4" "s2_tuplock4" "s1_commit"
