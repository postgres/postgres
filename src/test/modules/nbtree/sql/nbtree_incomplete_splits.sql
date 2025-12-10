--
-- Test incomplete splits in B-tree indexes.
--
-- We use a test table with integers from 1 to :next_i.  Each integer
-- occurs exactly once, no gaps or duplicates, although the index does
-- contain some duplicates because some of the inserting transactions
-- are rolled back during the test.  The exact contents of the table
-- depend on the physical layout of the index, which in turn depends
-- at least on the block size, so instead of checking the exact
-- contents, we check those invariants.  :next_i psql variable is
-- maintained at all times to hold the last inserted integer + 1.
--

-- This uses injection points to cause errors that leave some page
-- splits in "incomplete" state
set client_min_messages TO 'warning';
create extension if not exists injection_points;
create extension if not exists amcheck;
reset client_min_messages;

-- Make all injection points local to this process, for concurrency.
SELECT injection_points_set_local();

-- Use the index for all the queries
set enable_seqscan=off;

-- Print a NOTICE whenever an incomplete split gets fixed
SELECT injection_points_attach('nbtree-finish-incomplete-split', 'notice');

--
-- First create the test table and some helper functions
--
create table nbtree_incomplete_splits(i int4) with (autovacuum_enabled = off);

create index nbtree_incomplete_splits_i_idx on nbtree_incomplete_splits using btree (i);

-- Inserts 'n' rows to the test table. Pass :next_i as the first
-- argument, returns the new value for :next_i.
create function insert_n(first_i int, n int) returns int language plpgsql as $$
begin
  insert into nbtree_incomplete_splits select g from generate_series(first_i, first_i + n - 1) as g;
  return first_i + n;
end;
$$;

-- Inserts to the table until an insert fails. Like insert_n(), returns the
-- new value for :next_i.
create function insert_until_fail(next_i int, step int default 1) returns int language plpgsql as $$
declare
  i integer;
begin
  -- Insert rows in batches of 'step' rows each, until an error occurs.
  i := 0;
  loop
    begin
      select insert_n(next_i, step) into next_i;
    exception when others then
      raise notice 'failed with: %', sqlerrm;
      exit;
    end;

    -- The caller is expected to set an injection point that eventually
    -- causes an error. But bail out if still no error after 10000
    -- attempts, so that we don't get stuck in an infinite loop.
    i := i + 1;
    if i = 10000 then
      raise 'no error on inserts after % iterations', i;
    end if;
  end loop;

  return next_i;
end;
$$;

-- Check the invariants.
create function verify(next_i int) returns bool language plpgsql as $$
declare
  c integer;
begin
  -- Perform a scan over the trailing part of the index, where the
  -- possible incomplete splits are. (We don't check the whole table,
  -- because that'd be pretty slow.)
  --
  -- Find all rows that overlap with the last 200 inserted integers. Or
  -- the next 100, which shouldn't exist.
  select count(*) into c from nbtree_incomplete_splits where i between next_i - 200 and next_i + 100;
  if c <> 200 then
    raise 'unexpected count % ', c;
  end if;

  -- Also check the index with amcheck. Both to test that the index is
  -- valid, but also to test that amcheck doesn't wrongly complain
  -- about incomplete splits.
  perform bt_index_parent_check('nbtree_incomplete_splits_i_idx'::regclass, true, true);

  return true;
end;
$$;

-- Insert one array to get started.
select insert_n(1, 1000) as next_i
\gset
select verify(:next_i);


--
-- Test incomplete leaf split
--
SELECT injection_points_attach('nbtree-leave-leaf-split-incomplete', 'error');
select insert_until_fail(:next_i) as next_i
\gset
SELECT injection_points_detach('nbtree-leave-leaf-split-incomplete');

-- Verify that a scan works even though there's an incomplete split
select verify(:next_i);

-- Insert some more rows, finishing the split
select insert_n(:next_i, 10) as next_i
\gset
-- Verify that a scan still works
select verify(:next_i);


--
-- Test incomplete internal page split
--
SELECT injection_points_attach('nbtree-leave-internal-split-incomplete', 'error');
select insert_until_fail(:next_i, 100) as next_i
\gset
SELECT injection_points_detach('nbtree-leave-internal-split-incomplete');

 -- Verify that a scan works even though there's an incomplete split
select verify(:next_i);

-- Insert some more rows, finishing the split
select insert_n(:next_i, 10) as next_i
\gset
-- Verify that a scan still works
select verify(:next_i);

SELECT injection_points_detach('nbtree-finish-incomplete-split');

drop extension amcheck;
drop extension injection_points;
