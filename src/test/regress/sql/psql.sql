--
-- Tests for psql features that aren't closely connected to any
-- specific server features
--

-- \gset

select 10 as test01, 20 as test02, 'Hello' as test03 \gset pref01_

\echo :pref01_test01 :pref01_test02 :pref01_test03

-- should fail: bad variable name
select 10 as "bad name"
\gset

-- multiple backslash commands in one line
select 1 as x, 2 as y \gset pref01_ \\ \echo :pref01_x
select 3 as x, 4 as y \gset pref01_ \echo :pref01_x \echo :pref01_y
select 5 as x, 6 as y \gset pref01_ \\ \g \echo :pref01_x :pref01_y
select 7 as x, 8 as y \g \gset pref01_ \echo :pref01_x :pref01_y

-- NULL should unset the variable
\set var2 xyz
select 1 as var1, NULL as var2, 3 as var3 \gset
\echo :var1 :var2 :var3

-- \gset requires just one tuple
select 10 as test01, 20 as test02 from generate_series(1,3) \gset
select 10 as test01, 20 as test02 from generate_series(1,0) \gset

-- \gset should work in FETCH_COUNT mode too
\set FETCH_COUNT 1

select 1 as x, 2 as y \gset pref01_ \\ \echo :pref01_x
select 3 as x, 4 as y \gset pref01_ \echo :pref01_x \echo :pref01_y
select 10 as test01, 20 as test02 from generate_series(1,3) \gset
select 10 as test01, 20 as test02 from generate_series(1,0) \gset

\unset FETCH_COUNT

-- \gexec

create temporary table gexec_test(a int, b text, c date, d float);
select format('create index on gexec_test(%I)', attname)
from pg_attribute
where attrelid = 'gexec_test'::regclass and attnum > 0
order by attnum
\gexec

-- \gexec should work in FETCH_COUNT mode too
-- (though the fetch limit applies to the executed queries not the meta query)
\set FETCH_COUNT 1

select 'select 1 as ones', 'select x.y, x.y*2 as double from generate_series(1,4) as x(y)'
union all
select 'drop table gexec_test', NULL
union all
select 'drop table gexec_test', 'select ''2000-01-01''::date as party_over'
\gexec

\unset FETCH_COUNT

-- show all pset options
\pset

-- test multi-line headers, wrapping, and newline indicators
prepare q as select array_to_string(array_agg(repeat('x',2*n)),E'\n') as "ab

c", array_to_string(array_agg(repeat('y',20-2*n)),E'\n') as "a
bc" from generate_series(1,10) as n(n) group by n>1 ;

\pset linestyle ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset linestyle old-ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

deallocate q;

-- test single-line header and data
prepare q as select repeat('x',2*n) as "0123456789abcdef", repeat('y',20-2*n) as "0123456789" from generate_series(1,10) as n;

\pset linestyle ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset expanded on
\pset columns 30

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset expanded on
\pset columns 20

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset linestyle old-ascii

\pset expanded off
\pset columns 40

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset expanded on

\pset border 0
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 1
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

\pset border 2
\pset format unaligned
execute q;
\pset format aligned
execute q;
\pset format wrapped
execute q;

deallocate q;

\pset linestyle ascii

prepare q as select ' | = | lkjsafi\\/ /oeu rio)(!@&*#)*(!&@*) \ (&' as " | -- | 012345678 9abc def!*@#&!@(*&*~~_+-=\ \", '11' as "0123456789", 11 as int from generate_series(1,10) as n;

\pset format asciidoc
\pset expanded off
\pset border 0
execute q;

\pset border 1
execute q;

\pset border 2
execute q;

\pset expanded on
\pset border 0
execute q;

\pset border 1
execute q;

\pset border 2
execute q;

deallocate q;

\pset format aligned
\pset expanded off
\pset border 1

-- SHOW_CONTEXT

\set SHOW_CONTEXT never
do $$
begin
  raise notice 'foo';
  raise exception 'bar';
end $$;

\set SHOW_CONTEXT errors
do $$
begin
  raise notice 'foo';
  raise exception 'bar';
end $$;

\set SHOW_CONTEXT always
do $$
begin
  raise notice 'foo';
  raise exception 'bar';
end $$;

--
-- \crosstabview
--

CREATE TABLE ctv_data (v, h, c, i, d) AS
VALUES
   ('v1','h2','foo', 3, '2015-04-01'::date),
   ('v2','h1','bar', 3, '2015-01-02'),
   ('v1','h0','baz', NULL, '2015-07-12'),
   ('v0','h4','qux', 4, '2015-07-15'),
   ('v0','h4','dbl', -3, '2014-12-15'),
   ('v0',NULL,'qux', 5, '2014-07-15'),
   ('v1','h2','quux',7, '2015-04-04');

-- running \crosstabview after query uses query in buffer
SELECT v, EXTRACT(year FROM d), count(*)
 FROM ctv_data
 GROUP BY 1, 2
 ORDER BY 1, 2;
-- basic usage with 3 columns
 \crosstabview

-- ordered months in horizontal header, quoted column name
SELECT v, to_char(d, 'Mon') AS "month name", EXTRACT(month FROM d) AS num,
 count(*) FROM ctv_data  GROUP BY 1,2,3 ORDER BY 1
 \crosstabview v "month name":num 4

-- ordered months in vertical header, ordered years in horizontal header
SELECT EXTRACT(year FROM d) AS year, to_char(d,'Mon') AS "month name",
  EXTRACT(month FROM d) AS month,
  format('sum=%s avg=%s', sum(i), avg(i)::numeric(2,1))
  FROM ctv_data
  GROUP BY EXTRACT(year FROM d), to_char(d,'Mon'), EXTRACT(month FROM d)
ORDER BY month
\crosstabview "month name" year:year format

-- combine contents vertically into the same cell (V/H duplicates)
SELECT v, h, string_agg(c, E'\n') FROM ctv_data GROUP BY v, h ORDER BY 1,2,3
 \crosstabview 1 2 3

-- horizontal ASC order from window function
SELECT v,h, string_agg(c, E'\n') AS c, row_number() OVER(ORDER BY h) AS r
FROM ctv_data GROUP BY v, h ORDER BY 1,3,2
 \crosstabview v h:r c

-- horizontal DESC order from window function
SELECT v, h, string_agg(c, E'\n') AS c, row_number() OVER(ORDER BY h DESC) AS r
FROM ctv_data GROUP BY v, h ORDER BY 1,3,2
 \crosstabview v h:r c

-- horizontal ASC order from window function, NULLs pushed rightmost
SELECT v,h, string_agg(c, E'\n') AS c, row_number() OVER(ORDER BY h NULLS LAST) AS r
FROM ctv_data GROUP BY v, h ORDER BY 1,3,2
 \crosstabview v h:r c

-- only null, no column name, 2 columns: error
SELECT null,null \crosstabview

-- only null, no column name, 3 columns: works
SELECT null,null,null \crosstabview

-- null display
\pset null '#null#'
SELECT v,h, string_agg(i::text, E'\n') AS i FROM ctv_data
GROUP BY v, h ORDER BY h,v
 \crosstabview v h i
\pset null ''

-- refer to columns by position
SELECT v,h,string_agg(i::text, E'\n'), string_agg(c, E'\n')
FROM ctv_data GROUP BY v, h ORDER BY h,v
 \crosstabview 2 1 4

-- refer to columns by positions and names mixed
SELECT v,h, string_agg(i::text, E'\n') AS i, string_agg(c, E'\n') AS c
FROM ctv_data GROUP BY v, h ORDER BY h,v
 \crosstabview 1 "h" 4

-- error: bad column name
SELECT v,h,c,i FROM ctv_data
 \crosstabview v h j

-- error: bad column number
SELECT v,h,i,c FROM ctv_data
 \crosstabview 2 1 5

-- error: same H and V columns
SELECT v,h,i,c FROM ctv_data
 \crosstabview 2 h 4

-- error: too many columns
SELECT a,a,1 FROM generate_series(1,3000) AS a
 \crosstabview

-- error: only one column
SELECT 1 \crosstabview

DROP TABLE ctv_data;
