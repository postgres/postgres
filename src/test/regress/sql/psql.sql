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
