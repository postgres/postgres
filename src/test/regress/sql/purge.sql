--
-- PURGE
--
-- we did two updates on each of these 10K tables up above.  we should
-- therefore go from 10002 tuples (two of which are not visible without
-- using a time qual) to 10000.
--
-- vacuuming here also tests whether or not the hash index compaction
-- code works; this used to be commented out because the hash AM would
-- miss deleting a bunch of index tuples, which caused big problems when
-- you dereferenced the tids and found garbage..
--
-- absolute time
PURGE hash_f8_heap BEFORE 'now';

SELECT count(*) AS has_10002 FROM hash_f8_heap[,] h;

VACUUM hash_f8_heap;

SELECT count(*) AS has_10000 FROM hash_f8_heap[,] h;

-- relative time
PURGE hash_i4_heap AFTER '@ 1 second ago';

SELECT count(*) AS has_10002 FROM hash_i4_heap[,] h;

VACUUM hash_i4_heap;

SELECT count(*) AS has_10000 FROM hash_i4_heap[,] h;

