--
-- Test Partition pruning for HASH partitioning
-- We keep this as a seperate test as hash functions return
-- values will vary based on CPU architecture.
--

create table hp (a int, b text) partition by hash (a, b);
create table hp0 partition of hp for values with (modulus 4, remainder 0);
create table hp3 partition of hp for values with (modulus 4, remainder 3);
create table hp1 partition of hp for values with (modulus 4, remainder 1);
create table hp2 partition of hp for values with (modulus 4, remainder 2);

insert into hp values (null, null);
insert into hp values (1, null);
insert into hp values (1, 'xxx');
insert into hp values (null, 'xxx');
insert into hp values (10, 'xxx');
insert into hp values (10, 'yyy');
select tableoid::regclass, * from hp order by 1;

-- partial keys won't prune, nor would non-equality conditions
explain (costs off) select * from hp where a = 1;
explain (costs off) select * from hp where b = 'xxx';
explain (costs off) select * from hp where a is null;
explain (costs off) select * from hp where b is null;
explain (costs off) select * from hp where a < 1 and b = 'xxx';
explain (costs off) select * from hp where a <> 1 and b = 'yyy';

-- pruning should work if non-null values are provided for all the keys
explain (costs off) select * from hp where a is null and b is null;
explain (costs off) select * from hp where a = 1 and b is null;
explain (costs off) select * from hp where a = 1 and b = 'xxx';
explain (costs off) select * from hp where a is null and b = 'xxx';
explain (costs off) select * from hp where a = 10 and b = 'xxx';
explain (costs off) select * from hp where a = 10 and b = 'yyy';
explain (costs off) select * from hp where (a = 10 and b = 'yyy') or (a = 10 and b = 'xxx') or (a is null and b is null);

-- hash partitiong pruning doesn't occur with <> operator clauses
explain (costs off) select * from hp where a <> 1 and b <> 'xxx';

drop table hp;
