# When an update propagates a preexisting lock on the updated tuple, make sure
# we don't ignore the lock in subsequent operations of the new version.  (The
# version with the aborted savepoint uses a slightly different code path).
setup
{
	create table parent (i int, c char(3));
	create unique index parent_idx on parent (i);
	insert into parent values (1, 'AAA');
	create table child (i int references parent(i));
}

teardown
{
	drop table child, parent;
}

session s1
step s1b	{ BEGIN; }
step s1l	{ INSERT INTO child VALUES (1); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2l	{ INSERT INTO child VALUES (1); }
step s2c	{ COMMIT; }

session s3
step s3b	{ BEGIN; }
step s3u	{ UPDATE parent SET c=lower(c); }	# no key update
step s3u2	{ UPDATE parent SET i = i; }		# key update
step s3svu	{ SAVEPOINT f; UPDATE parent SET c = 'bbb'; ROLLBACK TO f; }
step s3d	{ DELETE FROM parent; }
step s3c	{ COMMIT; }

permutation s1b s1l s2b s2l s3b s3u          s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u  s3svu s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u2         s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u2 s3svu s3d s1c s2c s3c
permutation s1b s1l             s3b s3u          s3d s1c       s3c
permutation s1b s1l             s3b s3u  s3svu s3d s1c       s3c
permutation s1b s1l             s3b s3u2         s3d s1c       s3c
permutation s1b s1l             s3b s3u2 s3svu s3d s1c       s3c
