--
-- Miscellaneous topics
--

-- Verify that we can parse new-style CREATE FUNCTION/PROCEDURE
do
$$
  declare procedure int;  -- check we still recognize non-keywords as vars
  begin
  create function test1() returns int
    begin atomic
      select 2 + 2;
    end;
  create or replace procedure test2(x int)
    begin atomic
      select x + 2;
    end;
  end
$$;

\sf test1
\sf test2

-- Test %TYPE and %ROWTYPE error cases
create table misc_table(f1 int);

do $$ declare x foo%type; begin end $$;
do $$ declare x notice%type; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%type; begin end $$;
do $$ declare x foo.bar.baz%type; begin end $$;
do $$ declare x public.foo.bar%type; begin end $$;
do $$ declare x public.misc_table.zed%type; begin end $$;

do $$ declare x foo%rowtype; begin end $$;
do $$ declare x notice%rowtype; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%rowtype; begin end $$;
do $$ declare x foo.bar.baz%rowtype; begin end $$;
do $$ declare x public.foo%rowtype; begin end $$;
do $$ declare x public.misc_table%rowtype; begin end $$;
