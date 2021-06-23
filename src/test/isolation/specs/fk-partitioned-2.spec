# Make sure that FKs referencing partitioned tables actually work.
setup {
  drop table if exists ppk, pfk, pfk1;
  create table ppk (a int primary key) partition by list (a);
  create table ppk1 partition of ppk for values in (1);
  insert into ppk values (1);
  create table pfk (a int references ppk) partition by list (a);
  create table pfk1 partition of pfk for values in (1);
}

session s1
step s1b	{	begin; }
step s1d	{	delete from ppk where a = 1; }
step s1c	{	commit; }

session s2
step s2b	{	begin; }
step s2bs	{	begin isolation level serializable; select 1; }
step s2i	{	insert into pfk values (1); }
step s2c	{	commit; }

teardown	{	drop table ppk, pfk, pfk1; }

permutation s1b s1d  s2b  s2i s1c s2c
permutation s1b s1d  s2bs s2i s1c s2c
permutation s1b s2b  s1d  s2i s1c s2c
permutation s1b s2bs s1d  s2i s1c s2c
permutation s1b s2b  s2i  s1d s2c s1c
permutation s1b s2bs s2i  s1d s2c s1c
