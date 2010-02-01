--
-- AGGREGATES
--

SELECT avg(four) AS avg_1 FROM onek;

SELECT avg(a) AS avg_32 FROM aggtest WHERE a < 100;

-- In 7.1, avg(float4) is computed using float8 arithmetic.
-- Round the result to 3 digits to avoid platform-specific results.

SELECT avg(b)::numeric(10,3) AS avg_107_943 FROM aggtest;

SELECT avg(gpa) AS avg_3_4 FROM ONLY student;


SELECT sum(four) AS sum_1500 FROM onek;
SELECT sum(a) AS sum_198 FROM aggtest;
SELECT sum(b) AS avg_431_773 FROM aggtest;
SELECT sum(gpa) AS avg_6_8 FROM ONLY student;

SELECT max(four) AS max_3 FROM onek;
SELECT max(a) AS max_100 FROM aggtest;
SELECT max(aggtest.b) AS max_324_78 FROM aggtest;
SELECT max(student.gpa) AS max_3_7 FROM student;

SELECT stddev_pop(b) FROM aggtest;
SELECT stddev_samp(b) FROM aggtest;
SELECT var_pop(b) FROM aggtest;
SELECT var_samp(b) FROM aggtest;

SELECT stddev_pop(b::numeric) FROM aggtest;
SELECT stddev_samp(b::numeric) FROM aggtest;
SELECT var_pop(b::numeric) FROM aggtest;
SELECT var_samp(b::numeric) FROM aggtest;

-- population variance is defined for a single tuple, sample variance
-- is not
SELECT var_pop(1.0), var_samp(2.0);
SELECT stddev_pop(3.0::numeric), stddev_samp(4.0::numeric);

-- SQL2003 binary aggregates
SELECT regr_count(b, a) FROM aggtest;
SELECT regr_sxx(b, a) FROM aggtest;
SELECT regr_syy(b, a) FROM aggtest;
SELECT regr_sxy(b, a) FROM aggtest;
SELECT regr_avgx(b, a), regr_avgy(b, a) FROM aggtest;
SELECT regr_r2(b, a) FROM aggtest;
SELECT regr_slope(b, a), regr_intercept(b, a) FROM aggtest;
SELECT covar_pop(b, a), covar_samp(b, a) FROM aggtest;
SELECT corr(b, a) FROM aggtest;

SELECT count(four) AS cnt_1000 FROM onek;
SELECT count(DISTINCT four) AS cnt_4 FROM onek;

select ten, count(*), sum(four) from onek
group by ten order by ten;

select ten, count(four), sum(DISTINCT four) from onek
group by ten order by ten;

-- user-defined aggregates
SELECT newavg(four) AS avg_1 FROM onek;
SELECT newsum(four) AS sum_1500 FROM onek;
SELECT newcnt(four) AS cnt_1000 FROM onek;
SELECT newcnt(*) AS cnt_1000 FROM onek;
SELECT oldcnt(*) AS cnt_1000 FROM onek;
SELECT sum2(q1,q2) FROM int8_tbl;

-- test for outer-level aggregates

-- this should work
select ten, sum(distinct four) from onek a
group by ten
having exists (select 1 from onek b where sum(distinct a.four) = b.four);

-- this should fail because subquery has an agg of its own in WHERE
select ten, sum(distinct four) from onek a
group by ten
having exists (select 1 from onek b
               where sum(distinct a.four + b.four) = b.four);

-- Test handling of sublinks within outer-level aggregates.
-- Per bug report from Daniel Grace.
select
  (select max((select i.unique2 from tenk1 i where i.unique1 = o.unique1)))
from tenk1 o;

--
-- test for bitwise integer aggregates
--
CREATE TEMPORARY TABLE bitwise_test(
  i2 INT2,
  i4 INT4,
  i8 INT8,
  i INTEGER,
  x INT2,
  y BIT(4)
);

-- empty case
SELECT 
  BIT_AND(i2) AS "?",
  BIT_OR(i4)  AS "?"
FROM bitwise_test;

COPY bitwise_test FROM STDIN NULL 'null';
1	1	1	1	1	B0101
3	3	3	null	2	B0100
7	7	7	3	4	B1100
\.

SELECT
  BIT_AND(i2) AS "1",
  BIT_AND(i4) AS "1",
  BIT_AND(i8) AS "1",
  BIT_AND(i)  AS "?",
  BIT_AND(x)  AS "0",
  BIT_AND(y)  AS "0100",

  BIT_OR(i2)  AS "7",
  BIT_OR(i4)  AS "7",
  BIT_OR(i8)  AS "7",
  BIT_OR(i)   AS "?",
  BIT_OR(x)   AS "7",
  BIT_OR(y)   AS "1101"
FROM bitwise_test;

--
-- test boolean aggregates
--
-- first test all possible transition and final states

SELECT
  -- boolean and transitions
  -- null because strict
  booland_statefunc(NULL, NULL)  IS NULL AS "t",
  booland_statefunc(TRUE, NULL)  IS NULL AS "t",
  booland_statefunc(FALSE, NULL) IS NULL AS "t",
  booland_statefunc(NULL, TRUE)  IS NULL AS "t",
  booland_statefunc(NULL, FALSE) IS NULL AS "t",
  -- and actual computations
  booland_statefunc(TRUE, TRUE) AS "t",
  NOT booland_statefunc(TRUE, FALSE) AS "t",
  NOT booland_statefunc(FALSE, TRUE) AS "t",
  NOT booland_statefunc(FALSE, FALSE) AS "t";

SELECT
  -- boolean or transitions
  -- null because strict
  boolor_statefunc(NULL, NULL)  IS NULL AS "t",
  boolor_statefunc(TRUE, NULL)  IS NULL AS "t",
  boolor_statefunc(FALSE, NULL) IS NULL AS "t",
  boolor_statefunc(NULL, TRUE)  IS NULL AS "t",
  boolor_statefunc(NULL, FALSE) IS NULL AS "t",
  -- actual computations
  boolor_statefunc(TRUE, TRUE) AS "t",
  boolor_statefunc(TRUE, FALSE) AS "t",
  boolor_statefunc(FALSE, TRUE) AS "t",
  NOT boolor_statefunc(FALSE, FALSE) AS "t";

CREATE TEMPORARY TABLE bool_test(  
  b1 BOOL,
  b2 BOOL,
  b3 BOOL,
  b4 BOOL);

-- empty case
SELECT
  BOOL_AND(b1)   AS "n",
  BOOL_OR(b3)    AS "n"
FROM bool_test;

COPY bool_test FROM STDIN NULL 'null';
TRUE	null	FALSE	null
FALSE	TRUE	null	null
null	TRUE	FALSE	null
\.

SELECT
  BOOL_AND(b1)     AS "f",
  BOOL_AND(b2)     AS "t",
  BOOL_AND(b3)     AS "f",
  BOOL_AND(b4)     AS "n",
  BOOL_AND(NOT b2) AS "f",
  BOOL_AND(NOT b3) AS "t"
FROM bool_test;

SELECT
  EVERY(b1)     AS "f",
  EVERY(b2)     AS "t",
  EVERY(b3)     AS "f",
  EVERY(b4)     AS "n",
  EVERY(NOT b2) AS "f",
  EVERY(NOT b3) AS "t"
FROM bool_test;

SELECT
  BOOL_OR(b1)      AS "t",
  BOOL_OR(b2)      AS "t",
  BOOL_OR(b3)      AS "f",
  BOOL_OR(b4)      AS "n",
  BOOL_OR(NOT b2)  AS "f",
  BOOL_OR(NOT b3)  AS "t"
FROM bool_test;

--
-- Test several cases that should be optimized into indexscans instead of
-- the generic aggregate implementation.  We can't actually verify that they
-- are done as indexscans, but we can check that the results are correct.
--

-- Basic cases
select max(unique1) from tenk1;
select max(unique1) from tenk1 where unique1 < 42;
select max(unique1) from tenk1 where unique1 > 42;
select max(unique1) from tenk1 where unique1 > 42000;

-- multi-column index (uses tenk1_thous_tenthous)
select max(tenthous) from tenk1 where thousand = 33;
select min(tenthous) from tenk1 where thousand = 33;

-- check parameter propagation into an indexscan subquery
select f1, (select min(unique1) from tenk1 where unique1 > f1) AS gt
from int4_tbl;

-- check some cases that were handled incorrectly in 8.3.0
select distinct max(unique2) from tenk1;
select max(unique2) from tenk1 order by 1;
select max(unique2) from tenk1 order by max(unique2);
select max(unique2) from tenk1 order by max(unique2)+1;
select max(unique2), generate_series(1,3) as g from tenk1 order by g desc;

--
-- Test combinations of DISTINCT and/or ORDER BY
--

select array_agg(a order by b)
  from (values (1,4),(2,3),(3,1),(4,2)) v(a,b);
select array_agg(a order by a)
  from (values (1,4),(2,3),(3,1),(4,2)) v(a,b);
select array_agg(a order by a desc)
  from (values (1,4),(2,3),(3,1),(4,2)) v(a,b);
select array_agg(b order by a desc)
  from (values (1,4),(2,3),(3,1),(4,2)) v(a,b);

select array_agg(distinct a)
  from (values (1),(2),(1),(3),(null),(2)) v(a);
select array_agg(distinct a order by a)
  from (values (1),(2),(1),(3),(null),(2)) v(a);
select array_agg(distinct a order by a desc)
  from (values (1),(2),(1),(3),(null),(2)) v(a);
select array_agg(distinct a order by a desc nulls last)
  from (values (1),(2),(1),(3),(null),(2)) v(a);

-- multi-arg aggs, strict/nonstrict, distinct/order by

select aggfstr(a,b,c)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);
select aggfns(a,b,c)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);

select aggfstr(distinct a,b,c)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,3) i;
select aggfns(distinct a,b,c)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,3) i;

select aggfstr(distinct a,b,c order by b)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,3) i;
select aggfns(distinct a,b,c order by b)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,3) i;

-- test specific code paths

select aggfns(distinct a,a,c order by c using ~<~,a)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,2) i;
select aggfns(distinct a,a,c order by c using ~<~)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,2) i;
select aggfns(distinct a,a,c order by a)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,2) i;
select aggfns(distinct a,b,c order by a,c using ~<~,b)
  from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
       generate_series(1,2) i;

-- check node I/O via view creation and usage, also deparsing logic

create view agg_view1 as
  select aggfns(a,b,c)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(distinct a,b,c)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
         generate_series(1,3) i;

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(distinct a,b,c order by b)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
         generate_series(1,3) i;

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(a,b,c order by b+1)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(a,a,c order by b)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(a,b,c order by c using ~<~)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c);

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

create or replace view agg_view1 as
  select aggfns(distinct a,b,c order by a,c using ~<~,b)
    from (values (1,3,'foo'),(0,null,null),(2,2,'bar'),(3,1,'baz')) v(a,b,c),
         generate_series(1,2) i;

select * from agg_view1;
select pg_get_viewdef('agg_view1'::regclass);

drop view agg_view1;

-- incorrect DISTINCT usage errors

select aggfns(distinct a,b,c order by i)
  from (values (1,1,'foo')) v(a,b,c), generate_series(1,2) i;
select aggfns(distinct a,b,c order by a,b+1)
  from (values (1,1,'foo')) v(a,b,c), generate_series(1,2) i;
select aggfns(distinct a,b,c order by a,b,i,c)
  from (values (1,1,'foo')) v(a,b,c), generate_series(1,2) i;
select aggfns(distinct a,a,c order by a,b)
  from (values (1,1,'foo')) v(a,b,c), generate_series(1,2) i;

-- string_agg tests
select string_agg(a) from (values('aaaa'),('bbbb'),('cccc')) g(a);
select string_agg(a,',') from (values('aaaa'),('bbbb'),('cccc')) g(a);
select string_agg(a,',') from (values('aaaa'),(null),('bbbb'),('cccc')) g(a);
select string_agg(a,',') from (values(null),(null),('bbbb'),('cccc')) g(a);
select string_agg(a,',') from (values(null),(null)) g(a);
