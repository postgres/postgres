--
-- Test GIN indexes.
--
-- There are other tests to test different GIN opclassed. This is for testing
-- GIN itself.

-- Create and populate a test table with a GIN index.
create table gin_test_tbl(i int4[]);
create index gin_test_idx on gin_test_tbl using gin (i) with (fastupdate = on);
insert into gin_test_tbl select array[1, 2, g] from generate_series(1, 20000) g;
insert into gin_test_tbl select array[1, 3, g] from generate_series(1, 1000) g;

vacuum gin_test_tbl; -- flush the fastupdate buffers

-- Test vacuuming
delete from gin_test_tbl where i @> array[2];
vacuum gin_test_tbl;

-- Disable fastupdate, and do more insertions. With fastupdate enabled, most
-- insertions (by flushing the list pages) cause page splits. Without
-- fastupdate, we get more churn in the GIN data leaf pages, and exercise the
-- recompression codepaths.
alter index gin_test_idx set (fastupdate = off);

insert into gin_test_tbl select array[1, 2, g] from generate_series(1, 1000) g;
insert into gin_test_tbl select array[1, 3, g] from generate_series(1, 1000) g;

delete from gin_test_tbl where i @> array[2];
vacuum gin_test_tbl;
