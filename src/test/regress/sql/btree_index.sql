--
-- BTREE_INDEX
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

CREATE TABLE bt_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE bt_name_heap (
	seqno 		name,
	random 		int4
);

CREATE TABLE bt_txt_heap (
	seqno 		text,
	random 		int4
);

CREATE TABLE bt_f8_heap (
	seqno 		float8,
	random 		int4
);

\set filename :abs_srcdir '/data/desc.data'
COPY bt_i4_heap FROM :'filename';

\set filename :abs_srcdir '/data/hash.data'
COPY bt_name_heap FROM :'filename';

\set filename :abs_srcdir '/data/desc.data'
COPY bt_txt_heap FROM :'filename';

\set filename :abs_srcdir '/data/hash.data'
COPY bt_f8_heap FROM :'filename';

ANALYZE bt_i4_heap;
ANALYZE bt_name_heap;
ANALYZE bt_txt_heap;
ANALYZE bt_f8_heap;

--
-- BTREE ascending/descending cases
--
-- we load int4/text from pure descending data (each key is a new
-- low key) and name/f8 from pure ascending data (each key is a new
-- high key).  we had a bug where new low keys would sometimes be
-- "lost".
--
CREATE INDEX bt_i4_index ON bt_i4_heap USING btree (seqno int4_ops);

CREATE INDEX bt_name_index ON bt_name_heap USING btree (seqno name_ops);

CREATE INDEX bt_txt_index ON bt_txt_heap USING btree (seqno text_ops);

CREATE INDEX bt_f8_index ON bt_f8_heap USING btree (seqno float8_ops);

--
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
-- Add coverage of RowCompare quals whose row omits a column ("proargtypes")
-- that's after the first column, but before the final column.  The scan's
-- initial positioning strategy must become >= here (it's not the > strategy,
-- since the absence of "proargtypes" makes that tighter constraint unsafe).
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) > ('abs', 0)
ORDER BY proname, proargtypes, pronamespace LIMIT 1;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) > ('abs', 0)
ORDER BY proname, proargtypes, pronamespace LIMIT 1;

--
-- Similar to the previous test case, but this time it's a backwards scan
-- using a < RowCompare.  Must use the <= strategy (and not the < strategy).
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) < ('abs', 1_000_000)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC LIMIT 1;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) < ('abs', 1_000_000)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC LIMIT 1;

--
-- Add coverage for RowCompare quals whose rhs row has a NULL that ends scan
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'abs' AND (proname, proargtypes) < ('abs', NULL)
ORDER BY proname, proargtypes, pronamespace;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'abs' AND (proname, proargtypes) < ('abs', NULL)
ORDER BY proname, proargtypes, pronamespace;

--
-- Add coverage for backwards scan RowCompare quals whose rhs row has a NULL
-- that ends scan
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'abs' AND (proname, proargtypes) > ('abs', NULL)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'abs' AND (proname, proargtypes) > ('abs', NULL)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

--
-- Add coverage for recheck of > key following array advancement on previous
-- (left sibling) page that used a high key whose attribute value corresponding
-- to the > key was -inf (due to being truncated when the high key was created).
--
-- XXX This relies on the assumption that tenk1_thous_tenthous has a truncated
-- high key "(183, -inf)" on the first page that we'll scan.  The test will only
-- provide useful coverage when the default 8K BLCKSZ is in use.
--
explain (costs off)
SELECT thousand, tenthous
  FROM tenk1
  WHERE thousand IN (182, 183) AND tenthous > 7550;

SELECT thousand, tenthous
  FROM tenk1
  WHERE thousand IN (182, 183) AND tenthous > 7550;

--
-- Add coverage for optimization of backwards scan index descents
--
-- Here we expect _bt_search to descend straight to a leaf page containing a
-- non-pivot tuple with the value '47', which comes last (after 11 similar
-- non-pivot tuples).  Query execution should only need to visit a single
-- leaf page here.
--
-- Test case relies on tenk1_hundred index having a leaf page whose high key
-- is '(48, -inf)'.  We use a low cardinality index to make our test case less
-- sensitive to implementation details that may change in the future.
set enable_seqscan to false;
set enable_indexscan to true;
set enable_bitmapscan to false;
explain (costs off)
select hundred, twenty from tenk1 where hundred < 48 order by hundred desc limit 1;
select hundred, twenty from tenk1 where hundred < 48 order by hundred desc limit 1;

-- This variant of the query need only return a single tuple located to the immediate
-- right of the '(48, -inf)' high key.  It also only needs to scan one single
-- leaf page (the right sibling of the page scanned by the last test case):
explain (costs off)
select hundred, twenty from tenk1 where hundred <= 48 order by hundred desc limit 1;
select hundred, twenty from tenk1 where hundred <= 48 order by hundred desc limit 1;

--
-- Add coverage for ScalarArrayOp btree quals with pivot tuple constants
--
explain (costs off)
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82);
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82);

explain (costs off)
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82) order by hundred desc;
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82) order by hundred desc;

explain (costs off)
select thousand from tenk1 where thousand in (364, 366,380) and tenthous = 200000;
select thousand from tenk1 where thousand in (364, 366,380) and tenthous = 200000;

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

-- Also check LIKE optimization with binary-compatible cases

create temp table btree_bpchar (f1 text collate "C");
create index on btree_bpchar(f1 bpchar_ops) WITH (deduplicate_items=on);
insert into btree_bpchar values ('foo'), ('fool'), ('bar'), ('quux');
-- doesn't match index:
explain (costs off)
select * from btree_bpchar where f1 like 'foo';
select * from btree_bpchar where f1 like 'foo';
explain (costs off)
select * from btree_bpchar where f1 like 'foo%';
select * from btree_bpchar where f1 like 'foo%';
-- these do match the index:
explain (costs off)
select * from btree_bpchar where f1::bpchar like 'foo';
select * from btree_bpchar where f1::bpchar like 'foo';
explain (costs off)
select * from btree_bpchar where f1::bpchar like 'foo%';
select * from btree_bpchar where f1::bpchar like 'foo%';

-- get test coverage for "single value" deduplication strategy:
insert into btree_bpchar select 'foo' from generate_series(1,1500);

--
-- Perform unique checking, with and without the use of deduplication
--
CREATE TABLE dedup_unique_test_table (a int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX dedup_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=on);
CREATE UNIQUE INDEX plain_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=off);
-- Generate enough garbage tuples in index to ensure that even the unique index
-- with deduplication enabled has to check multiple leaf pages during unique
-- checking (at least with a BLCKSZ of 8192 or less)
DO $$
BEGIN
    FOR r IN 1..1350 LOOP
        DELETE FROM dedup_unique_test_table;
        INSERT INTO dedup_unique_test_table SELECT 1;
    END LOOP;
END$$;

-- Exercise the LP_DEAD-bit-set tuple deletion code with a posting list tuple.
-- The implementation prefers deleting existing items to merging any duplicate
-- tuples into a posting list, so we need an explicit test to make sure we get
-- coverage (note that this test also assumes BLCKSZ is 8192 or less):
DROP INDEX plain_unique;
DELETE FROM dedup_unique_test_table WHERE a = 1;
INSERT INTO dedup_unique_test_table SELECT i FROM generate_series(0,450) i;

--
-- Test B-tree fast path (cache rightmost leaf page) optimization.
--

-- First create a tree that's at least three levels deep (i.e. has one level
-- between the root and leaf levels). The text inserted is long.  It won't be
-- TOAST compressed because we use plain storage in the table.  Only a few
-- index tuples fit on each internal page, allowing us to get a tall tree with
-- few pages.  (A tall tree is required to trigger caching.)
--
-- The text column must be the leading column in the index, since suffix
-- truncation would otherwise truncate tuples on internal pages, leaving us
-- with a short tree.
create table btree_tall_tbl(id int4, t text);
alter table btree_tall_tbl alter COLUMN t set storage plain;
create index btree_tall_idx on btree_tall_tbl (t, id) with (fillfactor = 10);
insert into btree_tall_tbl select g, repeat('x', 250)
from generate_series(1, 130) g;
insert into btree_tall_tbl select g, NULL
from generate_series(50, 60) g;

--
-- Test for skip scan with type that lacks skip support (text)
--
set enable_seqscan to false;
set enable_bitmapscan to false;

-- Forwards scan
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t, id;
explain (costs off)
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t, id;

-- Backwards scan
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t DESC, id DESC;
explain (costs off)
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t DESC, id DESC;

reset enable_seqscan;
reset enable_bitmapscan;

--
-- Test for multilevel page deletion
--
CREATE TABLE delete_test_table (a bigint, b bigint, c bigint, d bigint);
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,80000) i;
ALTER TABLE delete_test_table ADD PRIMARY KEY (a,b,c,d);
-- Delete most entries, and vacuum, deleting internal pages and creating "fast
-- root"
DELETE FROM delete_test_table WHERE a < 79990;
VACUUM delete_test_table;

--
-- Test B-tree insertion with a metapage update (XLOG_BTREE_INSERT_META
-- WAL record type). This happens when a "fast root" page is split.  This
-- also creates coverage for nbtree FSM page recycling.
--
-- The vacuum above should've turned the leaf page into a fast root. We just
-- need to insert some rows to cause the fast root page to split.
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,1000) i;

-- Test unsupported btree opclass parameters
create index on btree_tall_tbl (id int4_ops(foo=1));

-- test parallel build with immutable function.
CREATE TABLE btree_test_expr (n int);
CREATE FUNCTION btree_test_func() RETURNS int LANGUAGE sql IMMUTABLE RETURN 0;
BEGIN;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL max_parallel_maintenance_workers = 4;
CREATE INDEX btree_test_expr_idx ON btree_test_expr USING btree (btree_test_func());
COMMIT;
DROP TABLE btree_test_expr;
DROP FUNCTION btree_test_func();

-- Test case of ALTER INDEX with abuse of column names for indexes.
-- This grammar is not officially supported, but the parser allows it.
CREATE INDEX btree_tall_idx2 ON btree_tall_tbl (id);
ALTER INDEX btree_tall_idx2 ALTER COLUMN id SET (n_distinct=100);
DROP INDEX btree_tall_idx2;
-- Partitioned index
CREATE TABLE btree_part (id int4) PARTITION BY RANGE (id);
CREATE INDEX btree_part_idx ON btree_part(id);
ALTER INDEX btree_part_idx ALTER COLUMN id SET (n_distinct=100);
DROP TABLE btree_part;
