--
-- This test checks that lookup_type_cache() can correctly handle an
-- interruption.  We use the injection point to simulate an error but note
-- that a similar situation could happen due to user query interruption.
-- Despite the interruption, a map entry from the relation oid to type cache
-- entry should be created.  This is validated by subsequent modification of
-- the table schema, then type casts which use new schema implying
-- successful type cache invalidation by relation oid.
--

CREATE EXTENSION injection_points;

-- Make all injection points local to this process, for concurrency.
SELECT injection_points_set_local();

CREATE TABLE t (i int);
SELECT injection_points_attach('typecache-before-rel-type-cache-insert', 'error');
SELECT '(1)'::t;
SELECT injection_points_detach('typecache-before-rel-type-cache-insert');
ALTER TABLE t ADD COLUMN j int;
SELECT '(1,2)'::t;
