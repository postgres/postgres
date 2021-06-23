# Test that detach partition concurrently makes the partition safe
# for foreign keys that reference it.

setup
{
  DROP TABLE IF EXISTS d_lp_fk, d_lp_fk_1, d_lp_fk_2, d_lp_fk_r;

  CREATE TABLE d_lp_fk (a int PRIMARY KEY) PARTITION BY LIST(a);
  CREATE TABLE d_lp_fk_1 PARTITION OF d_lp_fk FOR VALUES IN (1);
  CREATE TABLE d_lp_fk_2 PARTITION OF d_lp_fk FOR VALUES IN (2);
  INSERT INTO d_lp_fk VALUES (1), (2);

  CREATE TABLE d_lp_fk_r (a int references d_lp_fk);
}

teardown { DROP TABLE IF EXISTS d_lp_fk, d_lp_fk_1, d_lp_fk_2, d_lp_fk_r; }

session s1
step s1b		{ BEGIN; }
step s1s		{ SELECT * FROM d_lp_fk; }
step s1c		{ COMMIT; }

session s2
step s2d		{ ALTER TABLE d_lp_fk DETACH PARTITION d_lp_fk_1 CONCURRENTLY; }

session s3
step s3b		{ BEGIN; }
step s3i1		{ INSERT INTO d_lp_fk_r VALUES (1); }
step s3i2		{ INSERT INTO d_lp_fk_r VALUES (2); }
step s3c		{ COMMIT; }

# The transaction that detaches hangs until it sees any older transaction
# terminate.
permutation s1b s1s s2d s3i1 s1c
permutation s1b s1s s2d s3i2 s3i2 s1c

permutation s1b s1s s3i1 s2d s1c
permutation s1b s1s s3i2 s2d s1c

# what if s3 has an uncommitted insertion?
permutation s1b s1s s3b s2d s3i1 s1c s3c
