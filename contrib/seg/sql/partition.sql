--
-- Test that partitioned-index operations cope with objects that are
-- not in the secure search path.  (This has little to do with seg,
-- but we need an opclass that isn't in pg_catalog, and the base system
-- has no such opclass.)  Note that we need to test propagation of the
-- partitioned index's properties both to partitions that pre-date it
-- and to partitions created later.
--

create function mydouble(int) returns int strict immutable parallel safe
begin atomic select $1 * 2; end;

create collation mycollation from "POSIX";

create table pt (category int, sdata seg, tdata text)
  partition by list (category);

-- pre-existing partition
create table pt12 partition of pt for values in (1,2);

insert into pt values(1, '0 .. 1'::seg, 'zed');

-- expression references object in public schema
create index pti1 on pt ((mydouble(category) + 1));
-- opclass in public schema
create index pti2 on pt (sdata seg_ops);
-- collation in public schema
create index pti3 on pt (tdata collate mycollation);

-- new partition
create table pt34 partition of pt for values in (3,4);

insert into pt values(4, '-1 .. 1'::seg, 'foo');

\d+ pt
\d+ pt12
