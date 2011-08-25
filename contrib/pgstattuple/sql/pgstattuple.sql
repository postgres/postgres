CREATE EXTENSION pgstattuple;

--
-- It's difficult to come up with platform-independent test cases for
-- the pgstattuple functions, but the results for empty tables and
-- indexes should be that.
--

create table test (a int primary key);

select * from pgstattuple('test'::text);
select * from pgstattuple('test'::regclass);

select * from pgstatindex('test_pkey');

select pg_relpages('test');
select pg_relpages('test_pkey');
