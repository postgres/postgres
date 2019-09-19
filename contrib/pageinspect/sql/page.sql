CREATE EXTENSION pageinspect;

CREATE TABLE test1 (a int, b int);
INSERT INTO test1 VALUES (16777217, 131584);

VACUUM test1;  -- set up FSM

-- The page contents can vary, so just test that it can be read
-- successfully, but don't keep the output.

SELECT octet_length(get_raw_page('test1', 'main', 0)) AS main_0;
SELECT octet_length(get_raw_page('test1', 'main', 1)) AS main_1;

SELECT octet_length(get_raw_page('test1', 'fsm', 0)) AS fsm_0;
SELECT octet_length(get_raw_page('test1', 'fsm', 1)) AS fsm_1;

SELECT octet_length(get_raw_page('test1', 'vm', 0)) AS vm_0;
SELECT octet_length(get_raw_page('test1', 'vm', 1)) AS vm_1;

SELECT octet_length(get_raw_page('xxx', 'main', 0));
SELECT octet_length(get_raw_page('test1', 'xxx', 0));

SELECT get_raw_page('test1', 0) = get_raw_page('test1', 'main', 0);

SELECT pagesize, version FROM page_header(get_raw_page('test1', 0));

SELECT page_checksum(get_raw_page('test1', 0), 0) IS NOT NULL AS silly_checksum_test;

SELECT tuple_data_split('test1'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    FROM heap_page_items(get_raw_page('test1', 0));

SELECT * FROM fsm_page_contents(get_raw_page('test1', 'fsm', 0));

-- If we freeze the only tuple on test1, the infomask should
-- always be the same in all test runs. we show raw flags by
-- default: HEAP_XMIN_COMMITTED and HEAP_XMIN_INVALID.
VACUUM FREEZE test1;

SELECT t_infomask, t_infomask2, raw_flags, combined_flags
FROM heap_page_items(get_raw_page('test1', 0)),
     LATERAL heap_tuple_infomask_flags(t_infomask, t_infomask2);

-- output the decoded flag HEAP_XMIN_FROZEN instead
SELECT t_infomask, t_infomask2, raw_flags, combined_flags
FROM heap_page_items(get_raw_page('test1', 0)),
     LATERAL heap_tuple_infomask_flags(t_infomask, t_infomask2);

-- tests for decoding of combined flags
-- HEAP_XMAX_SHR_LOCK = (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)
SELECT * FROM heap_tuple_infomask_flags(x'0050'::int, 0);
-- HEAP_XMIN_FROZEN = (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)
SELECT * FROM heap_tuple_infomask_flags(x'0300'::int, 0);
-- HEAP_MOVED = (HEAP_MOVED_IN | HEAP_MOVED_OFF)
SELECT * FROM heap_tuple_infomask_flags(x'C000'::int, 0);
SELECT * FROM heap_tuple_infomask_flags(x'C000'::int, 0);

-- test all flags of t_infomask and t_infomask2
SELECT unnest(raw_flags)
  FROM heap_tuple_infomask_flags(x'FFFF'::int, x'FFFF'::int) ORDER BY 1;
SELECT unnest(combined_flags)
  FROM heap_tuple_infomask_flags(x'FFFF'::int, x'FFFF'::int) ORDER BY 1;

-- no flags at all
SELECT * FROM heap_tuple_infomask_flags(0, 0);
-- no combined flags
SELECT * FROM heap_tuple_infomask_flags(x'0010'::int, 0);

DROP TABLE test1;

-- check that using any of these functions with a partitioned table or index
-- would fail
create table test_partitioned (a int) partition by range (a);
create index test_partitioned_index on test_partitioned (a);
select get_raw_page('test_partitioned', 0); -- error about partitioned table
select get_raw_page('test_partitioned_index', 0); -- error about partitioned index

-- a regular table which is a member of a partition set should work though
create table test_part1 partition of test_partitioned for values from ( 1 ) to (100);
select get_raw_page('test_part1', 0); -- get farther and error about empty table
drop table test_partitioned;

-- check null bitmap alignment for table whose number of attributes is multiple of 8
create table test8 (f1 int, f2 int, f3 int, f4 int, f5 int, f6 int, f7 int, f8 int);
insert into test8(f1, f8) values (x'7f00007f'::int, 0);
select t_bits, t_data from heap_page_items(get_raw_page('test8', 0));
select tuple_data_split('test8'::regclass, t_data, t_infomask, t_infomask2, t_bits)
    from heap_page_items(get_raw_page('test8', 0));
drop table test8;
