# Tests for contrib/pgrowlocks

setup {
    CREATE TABLE multixact_conflict (a int PRIMARY KEY, b int);
    INSERT INTO multixact_conflict VALUES (1, 2), (3, 4);
}

teardown {
    DROP TABLE multixact_conflict;
}

session s1
step s1_begin { BEGIN; }
step s1_tuplock1 { SELECT * FROM multixact_conflict FOR KEY SHARE; }
step s1_tuplock2 { SELECT * FROM multixact_conflict FOR SHARE; }
step s1_tuplock3 { SELECT * FROM multixact_conflict FOR NO KEY UPDATE; }
step s1_tuplock4 { SELECT * FROM multixact_conflict FOR UPDATE; }
step s1_updatea { UPDATE multixact_conflict SET a = 10 WHERE a = 1; }
step s1_updateb { UPDATE multixact_conflict SET b = 11 WHERE b = 4; }
step s1_lcksvpt { SELECT * FROM multixact_conflict FOR KEY SHARE; SAVEPOINT s; }
step s1_commit { COMMIT; }

session s2
step s2_rowlocks { SELECT locked_row, multi, modes FROM pgrowlocks('multixact_conflict'); }

permutation s1_begin s1_tuplock1 s2_rowlocks s1_commit
permutation s1_begin s1_tuplock2 s2_rowlocks s1_commit
permutation s1_begin s1_tuplock3 s2_rowlocks s1_commit
permutation s1_begin s1_tuplock4 s2_rowlocks s1_commit
permutation s1_begin s1_updatea s2_rowlocks s1_commit
permutation s1_begin s1_updateb s2_rowlocks s1_commit

# test multixact cases using savepoints
permutation s1_begin s1_lcksvpt s1_tuplock1 s2_rowlocks s1_commit
permutation s1_begin s1_lcksvpt s1_tuplock2 s2_rowlocks s1_commit
permutation s1_begin s1_lcksvpt s1_tuplock3 s2_rowlocks s1_commit
permutation s1_begin s1_lcksvpt s1_tuplock4 s2_rowlocks s1_commit
permutation s1_begin s1_lcksvpt s1_updatea s2_rowlocks s1_commit
permutation s1_begin s1_lcksvpt s1_updateb s2_rowlocks s1_commit
