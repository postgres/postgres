# Verify that cloning a foreign key constraint to a partition ensures
# that referenced values exist, even if they're being concurrently
# deleted.
setup {
drop table if exists ppk, pfk, pfk1;
  create table ppk (a int primary key) partition by list (a);
  create table ppk1 partition of ppk for values in (1);
  insert into ppk values (1);
  create table pfk (a int references ppk) partition by list (a);
  create table pfk1 (a int not null);
  insert into pfk1 values (1);
}

session s1
step s1b	{	begin; }
step s1d	{	delete from ppk1 where a = 1; }
step s1c	{	commit; }

session s2
step s2b	{	begin; }
step s2a	{	alter table pfk attach partition pfk1 for values in (1); }
step s2c	{	commit; }

teardown	{	drop table ppk, pfk, pfk1; }

permutation s1b s1d s1c s2b s2a s2c
permutation s1b s1d s2b s1c s2a s2c
permutation s1b s1d s2b s2a s1c s2c
#permutation s1b s1d s2b s2a s2c s1c
permutation s1b s2b s1d s1c s2a s2c
permutation s1b s2b s1d s2a s1c s2c
#permutation s1b s2b s1d s2a s2c s1c
#permutation s1b s2b s2a s1d s1c s2c
permutation s1b s2b s2a s1d s2c s1c
permutation s1b s2b s2a s2c s1d s1c
permutation s2b s1b s1d s1c s2a s2c
permutation s2b s1b s1d s2a s1c s2c
#permutation s2b s1b s1d s2a s2c s1c
#permutation s2b s1b s2a s1d s1c s2c
permutation s2b s1b s2a s1d s2c s1c
permutation s2b s1b s2a s2c s1d s1c
#permutation s2b s2a s1b s1d s1c s2c
permutation s2b s2a s1b s1d s2c s1c
permutation s2b s2a s1b s2c s1d s1c
permutation s2b s2a s2c s1b s1d s1c
