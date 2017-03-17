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

DROP TABLE test1;

-- check that using any of these functions with a partitioned table would fail
create table test_partitioned (a int) partition by range (a);
select get_raw_page('test_partitioned', 0); -- error about partitioned table

-- a regular table which is a member of a partition set should work though
create table test_part1 partition of test_partitioned for values from ( 1 ) to (100);
select get_raw_page('test_part1', 0); -- get farther and error about empty table
drop table test_partitioned;
