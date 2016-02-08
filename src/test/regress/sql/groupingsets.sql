--
-- grouping sets
--

-- test data sources

create temp view gstest1(a,b,v)
  as values (1,1,10),(1,1,11),(1,2,12),(1,2,13),(1,3,14),
            (2,3,15),
            (3,3,16),(3,4,17),
            (4,1,18),(4,1,19);

create temp table gstest2 (a integer, b integer, c integer, d integer,
                           e integer, f integer, g integer, h integer);
copy gstest2 from stdin;
1	1	1	1	1	1	1	1
1	1	1	1	1	1	1	2
1	1	1	1	1	1	2	2
1	1	1	1	1	2	2	2
1	1	1	1	2	2	2	2
1	1	1	2	2	2	2	2
1	1	2	2	2	2	2	2
1	2	2	2	2	2	2	2
2	2	2	2	2	2	2	2
\.

create temp table gstest3 (a integer, b integer, c integer, d integer);
copy gstest3 from stdin;
1	1	1	1
2	2	2	2
\.
alter table gstest3 add primary key (a);

create temp table gstest_empty (a integer, b integer, v integer);

create function gstest_data(v integer, out a integer, out b integer)
  returns setof record
  as $f$
    begin
      return query select v, i from generate_series(1,3) i;
    end;
  $f$ language plpgsql;

-- basic functionality

-- simple rollup with multiple plain aggregates, with and without ordering
-- (and with ordering differing from grouping)
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b);
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by a,b;
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by b desc, a;
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by rollup (a,b) order by coalesce(a,0)+coalesce(b,0);

-- various types of ordered aggs
select a, b, grouping(a,b),
       array_agg(v order by v),
       string_agg(v::text, ':' order by v desc),
       percentile_disc(0.5) within group (order by v),
       rank(1,2,12) within group (order by a,b,v)
  from gstest1 group by rollup (a,b) order by a,b;

-- test usage of grouped columns in direct args of aggs
select grouping(a), a, array_agg(b),
       rank(a) within group (order by b nulls first),
       rank(a) within group (order by b nulls last)
  from (values (1,1),(1,4),(1,5),(3,1),(3,2)) v(a,b)
 group by rollup (a) order by a;

-- nesting with window functions
select a, b, sum(c), sum(sum(c)) over (order by a,b) as rsum
  from gstest2 group by rollup (a,b) order by rsum, a, b;

-- nesting with grouping sets
select sum(c) from gstest2
  group by grouping sets((), grouping sets((), grouping sets(())))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets((), grouping sets((), grouping sets(((a, b)))))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets(rollup(c), grouping sets(cube(c))))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(a, grouping sets(a, cube(b)))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets((a, (b))))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets((a, b)))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets(a, grouping sets(a), a))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets(grouping sets(a, grouping sets(a, grouping sets(a), ((a)), a, grouping sets(a), (a)), a))
  order by 1 desc;
select sum(c) from gstest2
  group by grouping sets((a,(a,b)), grouping sets((a,(a,b)),a))
  order by 1 desc;

-- empty input: first is 0 rows, second 1, third 3 etc.
select a, b, sum(v), count(*) from gstest_empty group by grouping sets ((a,b),a);
select a, b, sum(v), count(*) from gstest_empty group by grouping sets ((a,b),());
select a, b, sum(v), count(*) from gstest_empty group by grouping sets ((a,b),(),(),());
select sum(v), count(*) from gstest_empty group by grouping sets ((),(),());

-- empty input with joins tests some important code paths
select t1.a, t2.b, sum(t1.v), count(*) from gstest_empty t1, gstest_empty t2
 group by grouping sets ((t1.a,t2.b),());

-- simple joins, var resolution, GROUPING on join vars
select t1.a, t2.b, grouping(t1.a, t2.b), sum(t1.v), max(t2.a)
  from gstest1 t1, gstest2 t2
 group by grouping sets ((t1.a, t2.b), ());

select t1.a, t2.b, grouping(t1.a, t2.b), sum(t1.v), max(t2.a)
  from gstest1 t1 join gstest2 t2 on (t1.a=t2.a)
 group by grouping sets ((t1.a, t2.b), ());

select a, b, grouping(a, b), sum(t1.v), max(t2.c)
  from gstest1 t1 join gstest2 t2 using (a,b)
 group by grouping sets ((a, b), ());

-- check that functionally dependent cols are not nulled
select a, d, grouping(a,b,c)
  from gstest3
 group by grouping sets ((a,b), (a,c));

-- simple rescan tests

select a, b, sum(v.x)
  from (values (1),(2)) v(x), gstest_data(v.x)
 group by rollup (a,b);

select *
  from (values (1),(2)) v(x),
       lateral (select a, b, sum(v.x) from gstest_data(v.x) group by rollup (a,b)) s;

-- min max optimisation should still work with GROUP BY ()
explain (costs off)
  select min(unique1) from tenk1 GROUP BY ();

-- Views with GROUPING SET queries
CREATE VIEW gstest_view AS select a, b, grouping(a,b), sum(c), count(*), max(c)
  from gstest2 group by rollup ((a,b,c),(c,d));

select pg_get_viewdef('gstest_view'::regclass, true);

-- Nested queries with 3 or more levels of nesting
select(select (select grouping(a,b) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by (a,b)) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
select(select (select grouping(e,f) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by (a,b)) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
select(select (select grouping(c) from (values (1)) v2(c) GROUP BY c) from (values (1,2)) v1(a,b) group by (a,b)) from (values(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);

-- Combinations of operations
select a, b, c, d from gstest2 group by rollup(a,b),grouping sets(c,d);
select a, b from (values (1,2),(2,3)) v(a,b) group by a,b, grouping sets(a);

-- Tests for chained aggregates
select a, b, grouping(a,b), sum(v), count(*), max(v)
  from gstest1 group by grouping sets ((a,b),(a+1,b+1),(a+2,b+2));
select(select (select grouping(a,b) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by (a,b)) from (values(6,7)) v3(e,f) GROUP BY ROLLUP((e+1),(f+1));
select(select (select grouping(a,b) from (values (1)) v2(c)) from (values (1,2)) v1(a,b) group by (a,b)) from (values(6,7)) v3(e,f) GROUP BY CUBE((e+1),(f+1)) ORDER BY (e+1),(f+1);
select a, b, sum(c), sum(sum(c)) over (order by a,b) as rsum
  from gstest2 group by cube (a,b) order by rsum, a, b;
select a, b, sum(c) from (values (1,1,10),(1,1,11),(1,2,12),(1,2,13),(1,3,14),(2,3,15),(3,3,16),(3,4,17),(4,1,18),(4,1,19)) v(a,b,c) group by rollup (a,b);
select a, b, sum(v.x)
  from (values (1),(2)) v(x), gstest_data(v.x)
 group by cube (a,b) order by a,b;


-- Agg level check. This query should error out.
select (select grouping(a,b) from gstest2) from gstest2 group by a,b;

--Nested queries
select a, b, sum(c), count(*) from gstest2 group by grouping sets (rollup(a,b),a);

-- HAVING queries
select ten, sum(distinct four) from onek a
group by grouping sets((ten,four),(ten))
having exists (select 1 from onek b where sum(distinct a.four) = b.four);

-- Tests around pushdown of HAVING clauses, partially testing against previous bugs
select a,count(*) from gstest2 group by rollup(a) order by a;
select a,count(*) from gstest2 group by rollup(a) having a is distinct from 1 order by a;
explain (costs off)
  select a,count(*) from gstest2 group by rollup(a) having a is distinct from 1 order by a;

select v.c, (select count(*) from gstest2 group by () having v.c)
  from (values (false),(true)) v(c) order by v.c;
explain (costs off)
  select v.c, (select count(*) from gstest2 group by () having v.c)
    from (values (false),(true)) v(c) order by v.c;

-- HAVING with GROUPING queries
select ten, grouping(ten) from onek
group by grouping sets(ten) having grouping(ten) >= 0
order by 2,1;
select ten, grouping(ten) from onek
group by grouping sets(ten, four) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by rollup(ten) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by cube(ten) having grouping(ten) > 0
order by 2,1;
select ten, grouping(ten) from onek
group by (ten) having grouping(ten) >= 0
order by 2,1;

-- FILTER queries
select ten, sum(distinct four) filter (where four::text ~ '123') from onek a
group by rollup(ten);

-- More rescan tests
select * from (values (1),(2)) v(a) left join lateral (select v.a, four, ten, count(*) from onek group by cube(four,ten)) s on true order by v.a,four,ten;
select array(select row(v.a,s1.*) from (select two,four, count(*) from onek group by cube(two,four) order by two,four) s1) from (values (1),(2)) v(a);

-- Grouping on text columns
select sum(ten) from onek group by two, rollup(four::text) order by 1;
select sum(ten) from onek group by rollup(four::text), two order by 1;

-- end
