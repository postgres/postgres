--
-- BTREE_INDEX
-- test retrieval of min/max keys for each index
--

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno < 1;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno >= 9999;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno = 4500;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno < '1'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno >= '9999'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno = '4500'::name;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno < '1'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno >= '9999'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno = '4500'::text;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno < '1'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno >= '9999'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno = '4500'::float8;

--
-- Check correct optimization of LIKE (special index operator support)
-- for both indexscan and bitmapscan cases
--

set enable_seqscan to false;
set enable_indexscan to true;
set enable_bitmapscan to false;
explain (costs off)
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike '00%foo' order by 1;
select proname from pg_proc where proname ilike '00%foo' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike 'ri%foo' order by 1;

set enable_indexscan to false;
set enable_bitmapscan to true;
explain (costs off)
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike '00%foo' order by 1;
select proname from pg_proc where proname ilike '00%foo' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike 'ri%foo' order by 1;

reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;

--
-- Test B-tree fast path (cache rightmost leaf page) optimization.
--

-- First create a tree that's at least three levels deep (i.e. has one level
-- between the root and leaf levels). The text inserted is long.  It won't be
-- compressed because we use plain storage in the table.  Only a few index
-- tuples fit on each internal page, allowing us to get a tall tree with few
-- pages.  (A tall tree is required to trigger caching.)
--
-- The text column must be the leading column in the index, since suffix
-- truncation would otherwise truncate tuples on internal pages, leaving us
-- with a short tree.
create table btree_tall_tbl(id int4, t text);
alter table btree_tall_tbl alter COLUMN t set storage plain;
create index btree_tall_idx on btree_tall_tbl (t, id) with (fillfactor = 10);
insert into btree_tall_tbl select g, repeat('x', 250)
from generate_series(1, 130) g;

--
-- Test vacuum_cleanup_index_scale_factor
--

-- Simple create
create table btree_test(a int);
create index btree_idx1 on btree_test(a) with (vacuum_cleanup_index_scale_factor = 40.0);
select reloptions from pg_class WHERE oid = 'btree_idx1'::regclass;

-- Fail while setting improper values
create index btree_idx_err on btree_test(a) with (vacuum_cleanup_index_scale_factor = -10.0);
create index btree_idx_err on btree_test(a) with (vacuum_cleanup_index_scale_factor = 100.0);
create index btree_idx_err on btree_test(a) with (vacuum_cleanup_index_scale_factor = 'string');
create index btree_idx_err on btree_test(a) with (vacuum_cleanup_index_scale_factor = true);

-- Simple ALTER INDEX
alter index btree_idx1 set (vacuum_cleanup_index_scale_factor = 70.0);
select reloptions from pg_class WHERE oid = 'btree_idx1'::regclass;
