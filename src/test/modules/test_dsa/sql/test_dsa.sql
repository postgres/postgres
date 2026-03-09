CREATE EXTENSION test_dsa;

SELECT test_dsa_basic();
SELECT test_dsa_resowners();

-- Test allocations across a pre-defined range of pages.  This covers enough
-- range to check for the case of odd-sized segments, without making the test
-- too slow.
SELECT test_dsa_allocate(1001, 2000, 100);
-- Larger size with odd-sized segment.
SELECT test_dsa_allocate(6501, 6600, 100);
