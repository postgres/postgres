--
-- Tests for PL/pgSQL variable properties: CONSTANT, NOT NULL, initializers
--

create type var_record as (f1 int4, f2 int4);
create domain int_nn as int not null;
create domain var_record_nn as var_record not null;
create domain var_record_colnn as var_record check((value).f2 is not null);

-- CONSTANT

do $$
declare x constant int := 42;
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x constant int;
begin
  x := 42;  -- fail
end$$;

do $$
declare x constant int; y int;
begin
  for x, y in select 1, 2 loop  -- fail
  end loop;
end$$;

do $$
declare x constant int[];
begin
  x[1] := 42;  -- fail
end$$;

do $$
declare x constant int[]; y int;
begin
  for x[1], y in select 1, 2 loop  -- fail (currently, unsupported syntax)
  end loop;
end$$;

do $$
declare x constant var_record;
begin
  x.f1 := 42;  -- fail
end$$;

do $$
declare x constant var_record; y int;
begin
  for x.f1, y in select 1, 2 loop  -- fail
  end loop;
end$$;

-- initializer expressions

do $$
declare x int := sin(0);
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x int := 1/0;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x bigint[] := array[1,3,5];
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x record := row(1,2,3);
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record := row(1,2);
begin
  raise notice 'x = %', x;
end$$;

-- NOT NULL

do $$
declare x int not null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x int not null := 42;
begin
  raise notice 'x = %', x;
  x := null;  -- fail
end$$;

do $$
declare x int not null := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x record not null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x record not null := row(42);
begin
  raise notice 'x = %', x;
  x := row(null);  -- ok
  raise notice 'x = %', x;
  x := null;  -- fail
end$$;

do $$
declare x record not null := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record not null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record not null := row(41,42);
begin
  raise notice 'x = %', x;
  x := row(null,null);  -- ok
  raise notice 'x = %', x;
  x := null;  -- fail
end$$;

do $$
declare x var_record not null := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

-- Check that variables are reinitialized on block re-entry.

do $$
begin
  for i in 1..3 loop
    declare
      x int;
      y int := i;
      r record;
      c var_record;
    begin
      if i = 1 then
        x := 42;
        r := row(i, i+1);
        c := row(i, i+1);
      end if;
      raise notice 'x = %', x;
      raise notice 'y = %', y;
      raise notice 'r = %', r;
      raise notice 'c = %', c;
    end;
  end loop;
end$$;

-- Check enforcement of domain constraints during initialization

do $$
declare x int_nn;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x int_nn := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x int_nn := 42;
begin
  raise notice 'x = %', x;
  x := null;  -- fail
end$$;

do $$
declare x var_record_nn;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record_nn := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record_nn := row(1,2);
begin
  raise notice 'x = %', x;
  x := row(null,null);  -- ok
  x := null;  -- fail
end$$;

do $$
declare x var_record_colnn;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record_colnn := null;  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record_colnn := row(1,null);  -- fail
begin
  raise notice 'x = %', x;
end$$;

do $$
declare x var_record_colnn := row(1,2);
begin
  raise notice 'x = %', x;
  x := null;  -- fail
end$$;

do $$
declare x var_record_colnn := row(1,2);
begin
  raise notice 'x = %', x;
  x := row(null,null);  -- fail
end$$;
