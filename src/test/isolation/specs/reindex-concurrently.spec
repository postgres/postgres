# REINDEX CONCURRENTLY
#
# Ensure that concurrent operations work correctly when a REINDEX is performed
# concurrently.

setup
{
	CREATE TABLE reind_con_tab(id serial primary key, data text);
	INSERT INTO reind_con_tab(data) VALUES ('aa');
	INSERT INTO reind_con_tab(data) VALUES ('aaa');
	INSERT INTO reind_con_tab(data) VALUES ('aaaa');
	INSERT INTO reind_con_tab(data) VALUES ('aaaaa');
}

teardown
{
	DROP TABLE reind_con_tab;
}

session "s1"
setup { BEGIN; }
step "sel1" { SELECT data FROM reind_con_tab WHERE id = 3; }
step "end1" { COMMIT; }

session "s2"
setup { BEGIN; }
step "upd2" { UPDATE reind_con_tab SET data = 'bbbb' WHERE id = 3; }
step "ins2" { INSERT INTO reind_con_tab(data) VALUES ('cccc'); }
step "del2" { DELETE FROM reind_con_tab WHERE data = 'cccc'; }
step "end2" { COMMIT; }

session "s3"
step "reindex" { REINDEX TABLE CONCURRENTLY reind_con_tab; }

permutation "reindex" "sel1" "upd2" "ins2" "del2" "end1" "end2"
permutation "sel1" "reindex" "upd2" "ins2" "del2" "end1" "end2"
permutation "sel1" "upd2" "reindex" "ins2" "del2" "end1" "end2"
permutation "sel1" "upd2" "ins2" "reindex" "del2" "end1" "end2"
permutation "sel1" "upd2" "ins2" "del2" "reindex" "end1" "end2"
permutation "sel1" "upd2" "ins2" "del2" "end1" "reindex" "end2"
