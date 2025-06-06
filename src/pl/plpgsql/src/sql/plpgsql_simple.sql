--
-- Tests for plpgsql's handling of "simple" expressions
--

-- Check that changes to an inline-able function are handled correctly
create function simplesql(int) returns int language sql
as 'select $1';

create function simplecaller() returns int language plpgsql
as $$
declare
  sum int := 0;
begin
  for n in 1..10 loop
    sum := sum + simplesql(n);
    if n = 5 then
      create or replace function simplesql(int) returns int language sql
      as 'select $1 + 100';
    end if;
  end loop;
  return sum;
end$$;

select simplecaller();


-- Check that changes in search path are dealt with correctly
create schema simple1;

create function simple1.simpletarget(int) returns int language plpgsql
as $$begin return $1; end$$;

create function simpletarget(int) returns int language plpgsql
as $$begin return $1 + 100; end$$;

create or replace function simplecaller() returns int language plpgsql
as $$
declare
  sum int := 0;
begin
  for n in 1..10 loop
    sum := sum + simpletarget(n);
    if n = 5 then
      set local search_path = 'simple1';
    end if;
  end loop;
  return sum;
end$$;

select simplecaller();

-- try it with non-volatile functions, too
alter function simple1.simpletarget(int) immutable;
alter function simpletarget(int) immutable;

select simplecaller();

-- make sure flushing local caches changes nothing
\c -

select simplecaller();


-- Check case where first attempt to determine if it's simple fails

create function simplesql() returns int language sql
as $$select 1 / 0$$;

create or replace function simplecaller() returns int language plpgsql
as $$
declare x int;
begin
  select simplesql() into x;
  return x;
end$$;

select simplecaller();  -- division by zero occurs during simple-expr check

create or replace function simplesql() returns int language sql
as $$select 2 + 2$$;

select simplecaller();


-- Check case where called function changes from non-SRF to SRF (bug #18497)

create or replace function simplecaller() returns int language plpgsql
as $$
declare x int;
begin
  x := simplesql();
  return x;
end$$;

select simplecaller();

drop function simplesql();

create function simplesql() returns setof int language sql
as $$select 22 + 22$$;

select simplecaller();

select simplecaller();

-- Check handling of simple expression in a scrollable cursor (bug #18859)

do $$
declare
 p_CurData refcursor;
 val int;
begin
 open p_CurData scroll for select 42;
 fetch p_CurData into val;
 raise notice 'val = %', val;
end; $$;
