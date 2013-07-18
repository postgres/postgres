CREATE EXTENSION pgstattuple;

--
-- It's difficult to come up with platform-independent test cases for
-- the pgstattuple functions, but the results for empty tables and
-- indexes should be that.
--

create table test (a int primary key, b int[]);

select * from pgstattuple('test');
select * from pgstattuple('test'::text);
select * from pgstattuple('test'::name);
select * from pgstattuple('test'::regclass);
select pgstattuple(oid) from pg_class where relname = 'test';
select pgstattuple(relname) from pg_class where relname = 'test';

select * from pgstatindex('test_pkey');
select * from pgstatindex('test_pkey'::text);
select * from pgstatindex('test_pkey'::name);
select * from pgstatindex('test_pkey'::regclass);
select pgstatindex(oid) from pg_class where relname = 'test_pkey';
select pgstatindex(relname) from pg_class where relname = 'test_pkey';

select pg_relpages('test');
select pg_relpages('test_pkey');
select pg_relpages('test_pkey'::text);
select pg_relpages('test_pkey'::name);
select pg_relpages('test_pkey'::regclass);
select pg_relpages(oid) from pg_class where relname = 'test_pkey';
select pg_relpages(relname) from pg_class where relname = 'test_pkey';

create index test_ginidx on test using gin (b);

select * from pgstatginindex('test_ginidx');
