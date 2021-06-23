# Verify that default partition constraint is enforced correctly
# in light of partitions being added concurrently to its parent
setup {
  drop table if exists tpart;
  create table tpart(i int, j text) partition by range(i);
  create table tpart_1(like tpart);
  create table tpart_2(like tpart);
  create table tpart_default (a int, j text, i int) partition by list (j);
  create table tpart_default_default (a int, i int, b int, j text);
  alter table tpart_default_default drop b;
  alter table tpart_default attach partition tpart_default_default default;
  alter table tpart_default drop a;
  alter table tpart attach partition tpart_default default;
  alter table tpart attach partition tpart_1 for values from(0) to (100);
  insert into tpart_2 values (110,'xxx'), (120, 'yyy'), (150, 'zzz');
}

session s1
step s1b	{	begin; }
step s1a	{	alter table tpart attach partition tpart_2 for values from (100) to (200); }
step s1c	{	commit; }

session s2
step s2b	{	begin; }
step s2i	{	insert into tpart values (110,'xxx'), (120, 'yyy'), (150, 'zzz'); }
step s2i2	{	insert into tpart_default (i, j) values (110, 'xxx'), (120, 'yyy'), (150, 'zzz'); }
step s2c	{	commit; }
step s2s	{	select tableoid::regclass, * from tpart; }

teardown	{	drop table tpart; }

# insert into tpart by s2 which routes to tpart_default due to not seeing
# concurrently added tpart_2 should fail, because the partition constraint
# of tpart_default would have changed due to tpart_2 having been added
permutation s1b s1a s2b s2i s1c s2c s2s

# similar to above, but now insert into sub-partitioned tpart_default
permutation s1b s1a s2b s2i2 s1c s2c s2s

# reverse: now the insert into tpart_default by s2 occurs first followed by
# attach in s1, which should fail when it scans the leaf default partition
# find the violating rows
permutation s1b s2b s2i s1a s2c s1c s2s
