CREATE EXTENSION test_tidstore;

-- To hide the output of do_set_block_offsets()
CREATE TEMP TABLE hideblocks(blockno bigint);

-- Constant values used in the tests.
\set maxblkno 4294967295
-- The maximum number of heap tuples (MaxHeapTuplesPerPage) in 8kB block is 291.
-- We use a higher number to test tidstore.
\set maxoffset 512

SELECT test_create(false);

-- Test on empty tidstore.
SELECT test_is_full();
SELECT check_set_block_offsets();

-- Add TIDs.
INSERT INTO hideblocks (blockno)
SELECT do_set_block_offsets(blk, array_agg(off)::int2[])
  FROM
    (VALUES (0), (1), (:maxblkno / 2), (:maxblkno - 1), (:maxblkno)) AS blocks(blk),
    (VALUES (1), (2), (:maxoffset / 2), (:maxoffset - 1), (:maxoffset)) AS offsets(off)
  GROUP BY blk;

-- Test offsets embedded in the bitmap header.
SELECT do_set_block_offsets(501, array[greatest((random() * :maxoffset)::int, 1)]::int2[]);
SELECT do_set_block_offsets(502, array_agg(DISTINCT greatest((random() * :maxoffset)::int, 1))::int2[])
  FROM generate_series(1, 3);

-- Add enough TIDs to cause the store to appear "full", compared
-- to the allocated memory it started out with. This is easier
-- with memory contexts in local memory.
INSERT INTO hideblocks (blockno)
SELECT do_set_block_offsets(blk, ARRAY[1,31,32,63,64,200]::int2[])
  FROM generate_series(1000, 2000, 1) blk;

-- Zero offset not allowed
SELECT do_set_block_offsets(1, ARRAY[0]::int2[]);

-- Check TIDs we've added to the store.
SELECT check_set_block_offsets();

SELECT test_is_full();

-- Re-create the TID store for randommized tests.
SELECT test_destroy();


-- Test replacements crossing RT_CHILDPTR_IS_VALUE in both directions
SELECT test_create(false);
SELECT do_set_block_offsets(1, array[1]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2,3]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2,3,4]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2,3,4,100]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2,3,4]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2,3]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1,2]::int2[]); SELECT check_set_block_offsets();
SELECT do_set_block_offsets(1, array[1]::int2[]); SELECT check_set_block_offsets();
SELECT test_destroy();


-- Use shared memory this time. We can't do that in test_radixtree.sql,
-- because unused static functions would raise warnings there.
SELECT test_create(true);

-- Test offsets embedded in the bitmap header.
SELECT do_set_block_offsets(501, array[greatest((random() * :maxoffset)::int, 1)]::int2[]);
SELECT do_set_block_offsets(502, array_agg(DISTINCT greatest((random() * :maxoffset)::int, 1))::int2[])
  FROM generate_series(1, 3);

-- Random TIDs test. The offset numbers are randomized and must be
-- unique and ordered.
INSERT INTO hideblocks (blockno)
SELECT do_set_block_offsets(blkno, array_agg(DISTINCT greatest((random() * :maxoffset)::int, 1))::int2[])
  FROM generate_series(1, 100) num_offsets,
  generate_series(1000, 1100, 1) blkno
GROUP BY blkno;

-- Check TIDs we've added to the store.
SELECT check_set_block_offsets();

-- cleanup
SELECT test_destroy();
DROP TABLE hideblocks;
