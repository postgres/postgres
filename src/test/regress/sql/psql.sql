--
-- Tests for psql features that aren't closely connected to any
-- specific server features
--

-- \set

-- fail: invalid name
\set invalid/name foo
-- fail: invalid value for special variable
\set AUTOCOMMIT foo
\set FETCH_COUNT foo
-- check handling of built-in boolean variable
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK foo
\echo :ON_ERROR_ROLLBACK
\set ON_ERROR_ROLLBACK on
\echo :ON_ERROR_ROLLBACK
\unset ON_ERROR_ROLLBACK
\echo :ON_ERROR_ROLLBACK

-- \g and \gx

SELECT 1 as one, 2 as two \g
\gx
SELECT 3 as three, 4 as four \gx
\g

-- \gx should work in FETCH_COUNT mode too
\set FETCH_COUNT 1

SELECT 1 as one, 2 as two \g
\gx
SELECT 3 as three, 4 as four \gx
\g

\unset FETCH_COUNT

-- \gset

select 10 as test01, 20 as test02, 'Hello' as test03 \gset pref01_

\echo :pref01_test01 :pref01_test02 :pref01_test03

-- should fail: bad variable name
select 10 as "bad name"
\gset

select 97 as "EOF", 'ok' as _foo \gset IGNORE
\echo :IGNORE_foo :IGNOREEOF

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

-- \gdesc

SELECT
    NULL AS zero,
    1 AS one,
    2.0 AS two,
    'three' AS three,
    $1 AS four,
    sin($2) as five,
    'foo'::varchar(4) as six,
    CURRENT_DATE AS now
\gdesc

-- should work with tuple-returning utilities, such as EXECUTE
PREPARE test AS SELECT 1 AS first, 2 AS second;
EXECUTE test \gdesc
EXPLAIN EXECUTE test \gdesc

-- should fail cleanly - syntax error
SELECT 1 + \gdesc

-- check behavior with empty results
SELECT \gdesc
CREATE TABLE bububu(a int) \gdesc

-- subject command should not have executed
TABLE bububu;  -- fail

-- query buffer should remain unchanged
SELECT 1 AS x, 'Hello', 2 AS y, true AS "dirty\name"
\gdesc
\g

-- all on one line
SELECT 3 AS x, 'Hello', 4 AS y, true AS "dirty\name" \gdesc \g

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
bc" from generate_series(1,10) as n(n) group by n>1 order by n>1;

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

-- tests for \if ... \endif

\if true
  select 'okay';
  select 'still okay';
\else
  not okay;
  still not okay
\endif

-- at this point query buffer should still have last valid line
\g

-- \if should work okay on part of a query
select
  \if true
    42
  \else
    (bogus
  \endif
  forty_two;

select \if false \\ (bogus \else \\ 42 \endif \\ forty_two;

-- test a large nested if using a variety of true-equivalents
\if true
	\if 1
		\if yes
			\if on
				\echo 'all true'
			\else
				\echo 'should not print #1-1'
			\endif
		\else
			\echo 'should not print #1-2'
		\endif
	\else
		\echo 'should not print #1-3'
	\endif
\else
	\echo 'should not print #1-4'
\endif

-- test a variety of false-equivalents in an if/elif/else structure
\if false
	\echo 'should not print #2-1'
\elif 0
	\echo 'should not print #2-2'
\elif no
	\echo 'should not print #2-3'
\elif off
	\echo 'should not print #2-4'
\else
	\echo 'all false'
\endif

-- test simple true-then-else
\if true
	\echo 'first thing true'
\else
	\echo 'should not print #3-1'
\endif

-- test simple false-true-else
\if false
	\echo 'should not print #4-1'
\elif true
	\echo 'second thing true'
\else
	\echo 'should not print #5-1'
\endif

-- invalid boolean expressions are false
\if invalid boolean expression
	\echo 'will not print #6-1'
\else
	\echo 'will print anyway #6-2'
\endif

-- test un-matched endif
\endif

-- test un-matched else
\else

-- test un-matched elif
\elif

-- test double-else error
\if true
\else
\else
\endif

-- test elif out-of-order
\if false
\else
\elif
\endif

-- test if-endif matching in a false branch
\if false
    \if false
        \echo 'should not print #7-1'
    \else
        \echo 'should not print #7-2'
    \endif
    \echo 'should not print #7-3'
\else
    \echo 'should print #7-4'
\endif

-- show that vars and backticks are not expanded when ignoring extra args
\set foo bar
\echo :foo :'foo' :"foo"
\pset fieldsep | `nosuchcommand` :foo :'foo' :"foo"

-- show that vars and backticks are not expanded and commands are ignored
-- when in a false if-branch
\set try_to_quit '\\q'
\if false
	:try_to_quit
	\echo `nosuchcommand` :foo :'foo' :"foo"
	\pset fieldsep | `nosuchcommand` :foo :'foo' :"foo"
	\a \C arg1 \c arg1 arg2 arg3 arg4 \cd arg1 \conninfo
	\copy arg1 arg2 arg3 arg4 arg5 arg6
	\copyright \dt arg1 \e arg1 arg2
	\ef whole_line
	\ev whole_line
	\echo arg1 arg2 arg3 arg4 arg5 \echo arg1 \encoding arg1 \errverbose
	\g arg1 \gx arg1 \gexec \h \html \i arg1 \ir arg1 \l arg1 \lo arg1 arg2
	\o arg1 \p \password arg1 \prompt arg1 arg2 \pset arg1 arg2 \q
	\reset \s arg1 \set arg1 arg2 arg3 arg4 arg5 arg6 arg7 \setenv arg1 arg2
	\sf whole_line
	\sv whole_line
	\t arg1 \T arg1 \timing arg1 \unset arg1 \w arg1 \watch arg1 \x arg1
	-- \else here is eaten as part of OT_FILEPIPE argument
	\w |/no/such/file \else
	-- \endif here is eaten as part of whole-line argument
	\! whole_line \endif
\else
	\echo 'should print #8-1'
\endif

-- :{?...} defined variable test
\set i 1
\if :{?i}
  \echo '#9-1 ok, variable i is defined'
\else
  \echo 'should not print #9-2'
\endif

\if :{?no_such_variable}
  \echo 'should not print #10-1'
\else
  \echo '#10-2 ok, variable no_such_variable is not defined'
\endif

SELECT :{?i} AS i_is_defined;

SELECT NOT :{?no_such_var} AS no_such_var_is_not_defined;

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

-- test printing and clearing the query buffer
SELECT 1;
\p
SELECT 2 \r
\p
SELECT 3 \p
UNION SELECT 4 \p
UNION SELECT 5
ORDER BY 1;
\r
\p

-- tests for special result variables

-- working query, 2 rows selected
SELECT 1 AS stuff UNION SELECT 2;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT

-- syntax error
SELECT 1 UNION;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- empty query
;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
-- must have kept previous values
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- other query error
DROP TABLE this_table_does_not_exist;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- working \gdesc
SELECT 3 AS three, 4 AS four \gdesc
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT

-- \gdesc with an error
SELECT 4 AS \gdesc
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

-- check row count for a cursor-fetched query
\set FETCH_COUNT 10
select unique2 from tenk1 order by unique2 limit 19;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT

-- cursor-fetched query with an error after the first group
select 1/(15-unique2) from tenk1 order by unique2 limit 19;
\echo 'error:' :ERROR
\echo 'error code:' :SQLSTATE
\echo 'number of rows:' :ROW_COUNT
\echo 'last error message:' :LAST_ERROR_MESSAGE
\echo 'last error code:' :LAST_ERROR_SQLSTATE

\unset FETCH_COUNT
