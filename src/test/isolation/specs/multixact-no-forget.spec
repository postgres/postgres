# If transaction A holds a lock, and transaction B does an update,
# make sure we don't forget the lock if B aborts.
setup
{
  CREATE TABLE dont_forget (
	value	int
  );

  INSERT INTO dont_forget VALUES (1);
}

teardown
{
  DROP TABLE dont_forget;
}

session "s1"
setup			{ BEGIN; }
step "s1_show"	{ SELECT current_setting('default_transaction_isolation') <> 'read committed'; }
step "s1_lock"	{ SELECT * FROM dont_forget FOR KEY SHARE; }
step "s1_commit" { COMMIT; }

session "s2"
setup				{ BEGIN; }
step "s2_update"	{ UPDATE dont_forget SET value = 2; }
step "s2_abort"		{ ROLLBACK; }
step "s2_commit"	{ COMMIT; }

session "s3"
# try cases with both a non-conflicting lock with s1's and a conflicting one
step "s3_forkeyshr"	{ SELECT * FROM dont_forget FOR KEY SHARE; }
step "s3_fornokeyupd"	{ SELECT * FROM dont_forget FOR NO KEY UPDATE; }
step "s3_forupd"	{ SELECT * FROM dont_forget FOR UPDATE; }

permutation "s1_show" "s1_commit" "s2_commit"
permutation "s1_lock" "s2_update" "s2_abort" "s3_forkeyshr" "s1_commit"
permutation "s1_lock" "s2_update" "s2_commit" "s3_forkeyshr" "s1_commit"
permutation "s1_lock" "s2_update" "s1_commit" "s3_forkeyshr" "s2_commit"
permutation "s1_lock" "s2_update" "s2_abort" "s3_fornokeyupd" "s1_commit"
permutation "s1_lock" "s2_update" "s2_commit" "s3_fornokeyupd" "s1_commit"
permutation "s1_lock" "s2_update" "s1_commit" "s3_fornokeyupd" "s2_commit"
permutation "s1_lock" "s2_update" "s2_abort" "s3_forupd" "s1_commit"
permutation "s1_lock" "s2_update" "s2_commit" "s3_forupd" "s1_commit"
permutation "s1_lock" "s2_update" "s1_commit" "s3_forupd" "s2_commit"
