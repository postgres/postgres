--
-- Tests for PL/pgSQL handling of array variables
--
-- We also check arrays of composites here, so this has some overlap
-- with the plpgsql_record tests.
--

create type complex as (r float8, i float8);
create type quadarray as (c1 complex[], c2 complex);

do $$ declare a int[];
begin a := array[1,2]; a[3] := 4; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a[3] := 4; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a[1][4] := 4; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a[1] := 23::text; raise notice 'a = %', a; end$$;  -- lax typing

do $$ declare a int[];
begin a := array[1,2]; a[2:3] := array[3,4]; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a := array[1,2]; a[2] := a[2] + 1; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a[1:2] := array[3,4]; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a[1:2] := 4; raise notice 'a = %', a; end$$;  -- error

do $$ declare a complex[];
begin a[1] := (1,2); a[1].i := 11; raise notice 'a = %', a; end$$;

do $$ declare a complex[];
begin a[1].i := 11; raise notice 'a = %, a[1].i = %', a, a[1].i; end$$;

-- perhaps this ought to work, but for now it doesn't:
do $$ declare a complex[];
begin a[1:2].i := array[11,12]; raise notice 'a = %', a; end$$;

do $$ declare a quadarray;
begin a.c1[1].i := 11; raise notice 'a = %, a.c1[1].i = %', a, a.c1[1].i; end$$;

do $$ declare a int[];
begin a := array_agg(x) from (values(1),(2),(3)) v(x); raise notice 'a = %', a; end$$;

create temp table onecol as select array[1,2] as f1;

do $$ declare a int[];
begin a := f1 from onecol; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a := * from onecol for update; raise notice 'a = %', a; end$$;

-- error cases:

do $$ declare a int[];
begin a := from onecol; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a := f1, f1 from onecol; raise notice 'a = %', a; end$$;

insert into onecol values(array[11]);

do $$ declare a int[];
begin a := f1 from onecol; raise notice 'a = %', a; end$$;

do $$ declare a int[];
begin a := f1 from onecol limit 1; raise notice 'a = %', a; end$$;

do $$ declare a real;
begin a[1] := 2; raise notice 'a = %', a; end$$;

do $$ declare a complex;
begin a.r[1] := 2; raise notice 'a = %', a; end$$;
