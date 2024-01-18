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
