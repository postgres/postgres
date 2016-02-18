CREATE EXTENSION pgstattuple;

--
-- It's difficult to come up with platform-independent test cases for
-- the pgstattuple functions, but the results for empty tables and
-- indexes should be that.
--

create table test (a int primary key);

select * from pgstattuple('test'::text);
select * from pgstattuple('test'::regclass);

select version, tree_level,
    index_size / current_setting('block_size')::int as index_size,
    root_block_no, internal_pages, leaf_pages, empty_pages, deleted_pages,
    avg_leaf_density, leaf_fragmentation
    from pgstatindex('test_pkey');

select pg_relpages('test');
select pg_relpages('test_pkey');
