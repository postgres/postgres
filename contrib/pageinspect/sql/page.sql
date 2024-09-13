CREATE EXTENSION pageinspect;

CREATE TABLE test1 (a int, b int);
INSERT INTO test1 VALUES (16777217, 131584);

VACUUM (DISABLE_PAGE_SKIPPING) test1;  -- set up FSM

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

-- Failure with incorrect page size
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes.
\set VERBOSITY terse
SELECT fsm_page_contents('aaa'::bytea);
SELECT page_checksum('bbb'::bytea, 0);
SELECT page_header('ccc'::bytea);
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT fsm_page_contents(decode(repeat('00', :block_size), 'hex'));
SELECT page_header(decode(repeat('00', :block_size), 'hex'));
SELECT page_checksum(decode(repeat('00', :block_size), 'hex'), 1);

-- tests for sequences
create sequence test_sequence start 72057594037927937;
select tuple_data_split('test_sequence'::regclass, t_data, t_infomask, t_infomask2, t_bits)
  from heap_page_items(get_raw_page('test_sequence', 0));
drop sequence test_sequence;
