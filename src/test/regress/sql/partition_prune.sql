--
-- Test partitioning planner code
--
create table lp (a char) partition by list (a);
create table lp_default partition of lp default;
create table lp_ef partition of lp for values in ('e', 'f');
create table lp_ad partition of lp for values in ('a', 'd');
create table lp_bc partition of lp for values in ('b', 'c');
create table lp_g partition of lp for values in ('g');
create table lp_null partition of lp for values in (null);
explain (costs off) select * from lp;
explain (costs off) select * from lp where a > 'a' and a < 'd';
explain (costs off) select * from lp where a > 'a' and a <= 'd';
explain (costs off) select * from lp where a = 'a';
explain (costs off) select * from lp where 'a' = a;	/* commuted */
explain (costs off) select * from lp where a is not null;
explain (costs off) select * from lp where a is null;
explain (costs off) select * from lp where a = 'a' or a = 'c';
explain (costs off) select * from lp where a is not null and (a = 'a' or a = 'c');
explain (costs off) select * from lp where a <> 'g';
explain (costs off) select * from lp where a <> 'a' and a <> 'd';
explain (costs off) select * from lp where a not in ('a', 'd');

-- collation matches the partitioning collation, pruning works
create table coll_pruning (a text collate "C") partition by list (a);
create table coll_pruning_a partition of coll_pruning for values in ('a');
create table coll_pruning_b partition of coll_pruning for values in ('b');
create table coll_pruning_def partition of coll_pruning default;
explain (costs off) select * from coll_pruning where a collate "C" = 'a' collate "C";
-- collation doesn't match the partitioning collation, no pruning occurs
explain (costs off) select * from coll_pruning where a collate "POSIX" = 'a' collate "POSIX";

create table rlp (a int, b varchar) partition by range (a);
create table rlp_default partition of rlp default partition by list (a);
create table rlp_default_default partition of rlp_default default;
create table rlp_default_10 partition of rlp_default for values in (10);
create table rlp_default_30 partition of rlp_default for values in (30);
create table rlp_default_null partition of rlp_default for values in (null);
create table rlp1 partition of rlp for values from (minvalue) to (1);
create table rlp2 partition of rlp for values from (1) to (10);

create table rlp3 (b varchar, a int) partition by list (b varchar_ops);
create table rlp3_default partition of rlp3 default;
create table rlp3abcd partition of rlp3 for values in ('ab', 'cd');
create table rlp3efgh partition of rlp3 for values in ('ef', 'gh');
create table rlp3nullxy partition of rlp3 for values in (null, 'xy');
alter table rlp attach partition rlp3 for values from (15) to (20);

create table rlp4 partition of rlp for values from (20) to (30) partition by range (a);
create table rlp4_default partition of rlp4 default;
create table rlp4_1 partition of rlp4 for values from (20) to (25);
create table rlp4_2 partition of rlp4 for values from (25) to (29);

create table rlp5 partition of rlp for values from (31) to (maxvalue) partition by range (a);
create table rlp5_default partition of rlp5 default;
create table rlp5_1 partition of rlp5 for values from (31) to (40);

explain (costs off) select * from rlp where a < 1;
explain (costs off) select * from rlp where 1 > a;	/* commuted */
explain (costs off) select * from rlp where a <= 1;
explain (costs off) select * from rlp where a = 1;
explain (costs off) select * from rlp where a = 1::bigint;		/* same as above */
explain (costs off) select * from rlp where a = 1::numeric;	/* no pruning */
explain (costs off) select * from rlp where a <= 10;
explain (costs off) select * from rlp where a > 10;
explain (costs off) select * from rlp where a < 15;
explain (costs off) select * from rlp where a <= 15;
explain (costs off) select * from rlp where a > 15 and b = 'ab';
explain (costs off) select * from rlp where a = 16;
explain (costs off) select * from rlp where a = 16 and b in ('not', 'in', 'here');
explain (costs off) select * from rlp where a = 16 and b < 'ab';
explain (costs off) select * from rlp where a = 16 and b <= 'ab';
explain (costs off) select * from rlp where a = 16 and b is null;
explain (costs off) select * from rlp where a = 16 and b is not null;
explain (costs off) select * from rlp where a is null;
explain (costs off) select * from rlp where a is not null;
explain (costs off) select * from rlp where a > 30;
explain (costs off) select * from rlp where a = 30;	/* only default is scanned */
explain (costs off) select * from rlp where a <= 31;
explain (costs off) select * from rlp where a = 1 or a = 7;
explain (costs off) select * from rlp where a = 1 or b = 'ab';

explain (costs off) select * from rlp where a > 20 and a < 27;
explain (costs off) select * from rlp where a = 29;
explain (costs off) select * from rlp where a >= 29;

-- redundant clauses are eliminated
explain (costs off) select * from rlp where a > 1 and a = 10;	/* only default */
explain (costs off) select * from rlp where a > 1 and a >=15;	/* rlp3 onwards, including default */
explain (costs off) select * from rlp where a = 1 and a = 3;	/* empty */
explain (costs off) select * from rlp where (a = 1 and a = 3) or (a > 1 and a = 15);

-- multi-column keys
create table mc3p (a int, b int, c int) partition by range (a, abs(b), c);
create table mc3p_default partition of mc3p default;
create table mc3p0 partition of mc3p for values from (minvalue, minvalue, minvalue) to (1, 1, 1);
create table mc3p1 partition of mc3p for values from (1, 1, 1) to (10, 5, 10);
create table mc3p2 partition of mc3p for values from (10, 5, 10) to (10, 10, 10);
create table mc3p3 partition of mc3p for values from (10, 10, 10) to (10, 10, 20);
create table mc3p4 partition of mc3p for values from (10, 10, 20) to (10, maxvalue, maxvalue);
create table mc3p5 partition of mc3p for values from (11, 1, 1) to (20, 10, 10);
create table mc3p6 partition of mc3p for values from (20, 10, 10) to (20, 20, 20);
create table mc3p7 partition of mc3p for values from (20, 20, 20) to (maxvalue, maxvalue, maxvalue);

explain (costs off) select * from mc3p where a = 1;
explain (costs off) select * from mc3p where a = 1 and abs(b) < 1;
explain (costs off) select * from mc3p where a = 1 and abs(b) = 1;
explain (costs off) select * from mc3p where a = 1 and abs(b) = 1 and c < 8;
explain (costs off) select * from mc3p where a = 10 and abs(b) between 5 and 35;
explain (costs off) select * from mc3p where a > 10;
explain (costs off) select * from mc3p where a >= 10;
explain (costs off) select * from mc3p where a < 10;
explain (costs off) select * from mc3p where a <= 10 and abs(b) < 10;
explain (costs off) select * from mc3p where a = 11 and abs(b) = 0;
explain (costs off) select * from mc3p where a = 20 and abs(b) = 10 and c = 100;
explain (costs off) select * from mc3p where a > 20;
explain (costs off) select * from mc3p where a >= 20;
explain (costs off) select * from mc3p where (a = 1 and abs(b) = 1 and c = 1) or (a = 10 and abs(b) = 5 and c = 10) or (a > 11 and a < 20);
explain (costs off) select * from mc3p where (a = 1 and abs(b) = 1 and c = 1) or (a = 10 and abs(b) = 5 and c = 10) or (a > 11 and a < 20) or a < 1;
explain (costs off) select * from mc3p where (a = 1 and abs(b) = 1 and c = 1) or (a = 10 and abs(b) = 5 and c = 10) or (a > 11 and a < 20) or a < 1 or a = 1;
explain (costs off) select * from mc3p where a = 1 or abs(b) = 1 or c = 1;
explain (costs off) select * from mc3p where (a = 1 and abs(b) = 1) or (a = 10 and abs(b) = 10);
explain (costs off) select * from mc3p where (a = 1 and abs(b) = 1) or (a = 10 and abs(b) = 9);

-- a simpler multi-column keys case
create table mc2p (a int, b int) partition by range (a, b);
create table mc2p_default partition of mc2p default;
create table mc2p0 partition of mc2p for values from (minvalue, minvalue) to (1, minvalue);
create table mc2p1 partition of mc2p for values from (1, minvalue) to (1, 1);
create table mc2p2 partition of mc2p for values from (1, 1) to (2, minvalue);
create table mc2p3 partition of mc2p for values from (2, minvalue) to (2, 1);
create table mc2p4 partition of mc2p for values from (2, 1) to (2, maxvalue);
create table mc2p5 partition of mc2p for values from (2, maxvalue) to (maxvalue, maxvalue);

explain (costs off) select * from mc2p where a < 2;
explain (costs off) select * from mc2p where a = 2 and b < 1;
explain (costs off) select * from mc2p where a > 1;
explain (costs off) select * from mc2p where a = 1 and b > 1;

-- boolean partitioning
create table boolpart (a bool) partition by list (a);
create table boolpart_default partition of boolpart default;
create table boolpart_t partition of boolpart for values in ('true');
create table boolpart_f partition of boolpart for values in ('false');

explain (costs off) select * from boolpart where a in (true, false);
explain (costs off) select * from boolpart where a = false;
explain (costs off) select * from boolpart where not a = false;
explain (costs off) select * from boolpart where a is true or a is not true;
explain (costs off) select * from boolpart where a is not true;
explain (costs off) select * from boolpart where a is not true and a is not false;
explain (costs off) select * from boolpart where a is unknown;
explain (costs off) select * from boolpart where a is not unknown;

drop table lp, coll_pruning, rlp, mc3p, mc2p, boolpart;
