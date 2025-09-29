-- Tests for Bitmapsets
CREATE EXTENSION test_bitmapset;

-- bms_make_singleton()
SELECT test_bms_make_singleton(-1);
SELECT test_bms_make_singleton(42) AS result;
SELECT test_bms_make_singleton(0) AS result;
SELECT test_bms_make_singleton(1000) AS result;
-- Test module check
SELECT test_bms_make_singleton(NULL) AS result;

-- bms_add_member()
SELECT test_bms_add_member('(b 1)', -1); -- error
SELECT test_bms_add_member('(b)', -10); -- error
SELECT test_bms_add_member('(b)', 10) AS result;
SELECT test_bms_add_member('(b 5)', 10) AS result;
-- sort check
SELECT test_bms_add_member('(b 10)', 5) AS result;
-- idempotent change
SELECT test_bms_add_member('(b 10)', 10) AS result;
-- Test module check
SELECT test_bms_add_member('(b)', NULL) AS result;

-- bms_replace_members()
SELECT test_bms_replace_members(NULL, '(b 1 2 3)') AS result;
SELECT test_bms_replace_members('(b 1 2 3)', NULL) AS result;
SELECT test_bms_replace_members('(b 1 2 3)', '(b 3 5 6)') AS result;
SELECT test_bms_replace_members('(b 1 2 3)', '(b 3 5)') AS result;
SELECT test_bms_replace_members('(b 1 2)', '(b 3 5 7)') AS result;
-- Force repalloc() with larger set
SELECT test_bms_replace_members('(b 1 2 3 4 5)', '(b 500 600)') AS result;
-- Test module checks
SELECT test_bms_replace_members('(b 1 2 3)', NULL) AS result;
SELECT test_bms_replace_members('(b 5)', NULL) AS result;
SELECT test_bms_replace_members(NULL, '(b 5)') AS result;
SELECT test_bms_replace_members(NULL, NULL) AS result;

-- bms_del_member()
SELECT test_bms_del_member('(b)', -20); -- error
SELECT test_bms_del_member('(b)', 10) AS result;
SELECT test_bms_del_member('(b 10)', 10) AS result;
SELECT test_bms_del_member('(b 10)', 5) AS result;
SELECT test_bms_del_member('(b 1 2 3)', 2) AS result;
-- Reallocation check
SELECT test_bms_del_member(test_bms_del_member('(b 0 31 32 63 64)', 32), 63) AS result;
-- Word boundary
SELECT test_bms_del_member(test_bms_add_range('(b)', 30, 34), 32) AS result;
-- Force word count changes
SELECT test_bms_del_member('(b 1 200)', 200) AS result;
SELECT test_bms_del_member('(b 1 50 100 200)', 200) AS result;
SELECT test_bms_del_member('(b 1 50 100 200)', 100) AS result;
-- Test module checks
SELECT test_bms_del_member('(b 42)', 42) AS result;
SELECT test_bms_del_member('(b 5)', NULL) AS result;

-- bms_del_members()
SELECT test_bms_del_members('(b)', '(b 10)') AS result;
SELECT test_bms_del_members('(b 10)', '(b 10)') AS result;
SELECT test_bms_del_members('(b 10)', '(b 5)') AS result;
SELECT test_bms_del_members('(b 1 2 3)', '(b 2)') AS result;
SELECT test_bms_del_members('(b 5 100)', '(b 100)') AS result;
SELECT test_bms_del_members('(b 5 100 200)', '(b 200)') AS result;
-- Force word count changes
SELECT test_bms_del_members('(b 1 2 100 200 300)', '(b 1 2)') AS result;
SELECT test_bms_del_members('(b 1 2 100 200 300)', '(b 200 300)') AS result;
-- Test module checks
SELECT test_bms_del_members('(b 5)', NULL) AS result;
SELECT test_bms_del_members(NULL, '(b 5)') AS result;

-- bms_join()
SELECT test_bms_join('(b 1 3 5)', NULL) AS result;
SELECT test_bms_join(NULL, '(b 2 4 6)') AS result;
SELECT test_bms_join('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_join('(b 1 3 5)', '(b 1 4 5)') AS result;
-- Force word count changes
SELECT test_bms_join('(b 5)', '(b 100)') AS result;
SELECT test_bms_join('(b 1 2)', '(b 100 200 300)') AS result;
-- Test module checks
SELECT test_bms_join('(b 5)', NULL) AS result;
SELECT test_bms_join(NULL, '(b 5)') AS result;
SELECT test_bms_join(NULL, NULL) AS result;

-- bms_union()
-- Overlapping sets
SELECT test_bms_union('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Union with NULL
SELECT test_bms_union('(b 1 3 5)', '(b)') AS result;
-- Union of empty with empty
SELECT test_bms_union('(b)', '(b)') AS result;
-- Overlapping ranges
SELECT test_bms_union(
         test_bms_add_range('(b)', 0, 15),
         test_bms_add_range('(b)', 10, 20)
       ) AS result;
-- Union with varrying word counts
SELECT test_bms_union('(b 1 2)', '(b 100 300)') AS result;
SELECT test_bms_union('(b 100 300)', '(b 1 2)') AS result;
-- Test module checks
SELECT test_bms_union('(b 5)', NULL) AS result;
SELECT test_bms_union(NULL, '(b 5)') AS result;
SELECT test_bms_union(NULL, NULL) AS result;

-- bms_intersect()
-- Overlapping sets
SELECT test_bms_intersect('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Disjoint sets
SELECT test_bms_intersect('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Intersect with empty
SELECT test_bms_intersect('(b 1 3 5)', '(b)') AS result;
-- Intersect with varrying word counts
SELECT test_bms_intersect('(b 1 300)', '(b 1 2 3 4 5)') AS result;
SELECT test_bms_intersect('(b 1 2 3 4 5)', '(b 1 300)') AS result;
-- Test module checks
SELECT test_bms_intersect('(b 1)', '(b 2)') AS result;
SELECT test_bms_intersect('(b 5)', NULL) AS result;
SELECT test_bms_intersect(NULL, '(b 5)') AS result;
SELECT test_bms_intersect(NULL, NULL) AS result;

-- bms_int_members()
-- Overlapping sets
SELECT test_bms_int_members('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Disjoint sets
SELECT test_bms_int_members('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Intersect with empty
SELECT test_bms_int_members('(b 1 3 5)', '(b)') AS result;
-- Multiple members
SELECT test_bms_int_members('(b 0 31 32 63 64)', '(b 31 32 64 65)') AS result;
-- Test module checks
SELECT test_bms_int_members('(b 1)', '(b 2)') AS result;
SELECT test_bms_int_members('(b 5)', NULL) AS result;
SELECT test_bms_int_members(NULL, '(b 5)') AS result;
SELECT test_bms_int_members(NULL, NULL) AS result;

-- bms_difference()
-- Overlapping sets
SELECT test_bms_difference('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Disjoint sets
SELECT test_bms_difference('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Identical sets
SELECT test_bms_difference('(b 1 3 5)', '(b 1 3 5)') AS result;
-- Substraction to empty
SELECT test_bms_difference('(b 42)', '(b 42)') AS result;
-- Subtraction edge case
SELECT test_bms_difference(
         test_bms_add_range('(b)', 0, 100),
         test_bms_add_range('(b)', 50, 150)
       ) AS result;
-- Difference with different word counts
SELECT test_bms_difference('(b 5 100)', '(b 5)') AS result;
SELECT test_bms_difference('(b 1 2 100 200)', '(b 1 2)') AS result;
-- Test module checks
SELECT test_bms_difference('(b 5)', '(b 5 10)') AS result;
SELECT test_bms_difference('(b 5)', NULL) AS result;
SELECT test_bms_difference(NULL, '(b 5)') AS result;
SELECT test_bms_difference(NULL, NULL) AS result;

-- bms_is_member()
SELECT test_bms_is_member('(b)', -5); -- error
SELECT test_bms_is_member('(b 1 3 5)', 1) AS result;
SELECT test_bms_is_member('(b 1 3 5)', 2) AS result;
SELECT test_bms_is_member('(b 1 3 5)', 3) AS result;
SELECT test_bms_is_member('(b)', 1) AS result;
-- Test module check
SELECT test_bms_is_member('(b 5)', NULL) AS result;

-- bms_member_index()
SELECT test_bms_member_index(NULL, 1) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 2) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 1) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 3) AS result;
-- Member index with various word positions
SELECT test_bms_member_index('(b 100 200)', 100) AS result;
SELECT test_bms_member_index('(b 100 200)', 200) AS result;
SELECT test_bms_member_index('(b 1 50 100 200)', 200) AS result;
-- Test module checks
SELECT test_bms_member_index('', 5) AS result;
SELECT test_bms_member_index(NULL, 5) AS result;

-- bms_num_members()
SELECT test_bms_num_members('(b)') AS result;
SELECT test_bms_num_members('(b 1 3 5)') AS result;
SELECT test_bms_num_members('(b 2 4 6 8 10)') AS result;

-- test_bms_equal()
SELECT test_bms_equal('(b)', '(b)') AS result;
SELECT test_bms_equal('(b)', '(b 1 3 5)') AS result;
SELECT test_bms_equal('(b 1 3 5)', '(b)') AS result;
SELECT test_bms_equal('(b 1 3 5)', '(b 1 3 5)') AS result;
SELECT test_bms_equal('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Equal with different word counts
SELECT test_bms_equal('(b 5)', '(b 100)') AS result;
SELECT test_bms_equal('(b 5 10)', '(b 100 200 300)') AS result;
-- Test module checks
SELECT test_bms_equal('(b 5)', NULL) AS result;
SELECT test_bms_equal(NULL, '(b 5)') AS result;
SELECT test_bms_equal(NULL, NULL) AS result;

-- bms_compare()
SELECT test_bms_compare('(b)', '(b)') AS result;
SELECT test_bms_compare('(b)', '(b 1 3)') AS result;
SELECT test_bms_compare('(b 1 3)', '(b)') AS result;
SELECT test_bms_compare('(b 1 3)', '(b 1 3)') AS result;
SELECT test_bms_compare('(b 1 3)', '(b 1 3 5)') AS result;
SELECT test_bms_compare('(b 1 3 5)', '(b 1 3)') AS result;
SELECT test_bms_compare(
         test_bms_add_range('(b)', 0, 63),
         test_bms_add_range('(b)', 0, 64)
       ) AS result;
-- Test module checks
SELECT test_bms_compare('(b 5)', NULL) AS result;
SELECT test_bms_compare(NULL, '(b 5)') AS result;
SELECT test_bms_compare(NULL, NULL) AS result;

-- bms_add_range()
SELECT test_bms_add_range('(b)', -5, 10); -- error
SELECT test_bms_add_range('(b)', 5, 7) AS result;
SELECT test_bms_add_range('(b)', 5, 5) AS result;
SELECT test_bms_add_range('(b 1 10)', 5, 7) AS result;
-- Word boundary of 31
SELECT test_bms_add_range('(b)', 30, 34) AS result;
-- Word boundary of 63
SELECT test_bms_add_range('(b)', 62, 66) AS result;
-- Large range
SELECT length(test_bms_add_range('(b)', 0, 1000)) AS result;
-- Force reallocations
SELECT length(test_bms_add_range('(b)', 0, 200)) AS result;
SELECT length(test_bms_add_range('(b)', 1000, 1100)) AS result;
-- Force word count expansion
SELECT test_bms_add_range('(b 5)', 100, 105) AS result;
SELECT length(test_bms_add_range('(b 1 2)', 200, 250)) AS result;
-- Test module checks
SELECT test_bms_add_range('(b 5)', 5, NULL) AS result;
SELECT test_bms_add_range('(b 5)', NULL, 10) AS result;
SELECT test_bms_add_range('(b 5)', NULL, NULL) AS result;
SELECT test_bms_add_range(NULL, 5, 10) AS result;
SELECT test_bms_add_range(NULL, 10, 5) AS result;
SELECT test_bms_add_range(NULL, NULL, NULL) AS result;

-- bms_membership()
SELECT test_bms_membership('(b)') AS result;
SELECT test_bms_membership('(b 42)') AS result;
SELECT test_bms_membership('(b 1 2)') AS result;
-- Test module check
SELECT test_bms_membership(NULL) AS result;

-- bms_is_empty()
SELECT test_bms_is_empty(NULL) AS result;
SELECT test_bms_is_empty('(b)') AS result;
SELECT test_bms_is_empty('(b 1)') AS result;
-- Test module check
SELECT test_bms_is_empty(NULL) AS result;

-- bms_singleton_member()
SELECT test_bms_singleton_member('(b)'); -- error
SELECT test_bms_singleton_member('(b 1 2)'); -- error
SELECT test_bms_singleton_member('(b 42)') AS result;
-- Test module check
SELECT test_bms_singleton_member(NULL) AS result;

-- bms_get_singleton_member()
SELECT test_bms_get_singleton_member('(b)', 1000);
-- Not a singleton, returns input default
SELECT test_bms_get_singleton_member('(b 3 6)', 1000) AS result;
-- Singletone, returns sole member
SELECT test_bms_get_singleton_member('(b 400)', 1000) AS result;
-- Test module checks
SELECT test_bms_get_singleton_member('', 1000) AS result;
SELECT test_bms_get_singleton_member(NULL, -1) AS result;

-- bms_next_member() and bms_prev_member()
-- First member
SELECT test_bms_next_member('(b 5 10 15 20)', -1) AS result;
-- Second member
SELECT test_bms_next_member('(b 5 10 15 20)', 5) AS result;
-- Member past the end
SELECT test_bms_next_member('(b 5 10 15 20)', 20) AS result;
-- Empty set
SELECT test_bms_next_member('(b)', -1) AS result;
-- Last member
SELECT test_bms_prev_member('(b 5 10 15 20)', 21) AS result;
-- Penultimate member
SELECT test_bms_prev_member('(b 5 10 15 20)', 20) AS result;
-- Past beginning member
SELECT test_bms_prev_member('(b 5 10 15 20)', 5) AS result;
-- Empty set
SELECT test_bms_prev_member('(b)', 100) AS result;
-- Negative prevbit should result in highest possible bit in set
SELECT test_bms_prev_member('(b 0 63 64 127)', -1) AS result;
-- Test module checks
SELECT test_bms_next_member('', 5) AS result;
SELECT test_bms_next_member('(b 5)', NULL) AS result;
SELECT test_bms_next_member(NULL, 5) AS result;
SELECT test_bms_next_member(NULL, NULL) AS result;
SELECT test_bms_prev_member('', 5) AS result;
SELECT test_bms_prev_member('(b 5)', NULL) AS result;
SELECT test_bms_prev_member(NULL, 5) AS result;

-- bms_hash_value()
SELECT test_bms_hash_value('(b)') = 0 AS result;
SELECT test_bms_hash_value('(b 1 3 5)') = test_bms_hash_value('(b 1 3 5)') AS result;
SELECT test_bms_hash_value('(b 1 3 5)') != test_bms_hash_value('(b 2 4 6)') AS result;
-- Test module check
SELECT test_bms_hash_value(NULL) AS result;

-- bms_overlap()
SELECT test_bms_overlap('(b 1 3 5)', '(b 3 5 7)') AS result;
SELECT test_bms_overlap('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_overlap('(b)', '(b 1 3 5)') AS result;
-- Test module checks
SELECT test_bms_overlap('(b 5)', NULL) AS result;
SELECT test_bms_overlap(NULL, '(b 5)') AS result;
SELECT test_bms_overlap(NULL, NULL) AS result;

-- bms_is_subset()
SELECT test_bms_is_subset('(b)', '(b 1 3 5)') AS result;
SELECT test_bms_is_subset('(b 1 3)', '(b 1 3 5)') AS result;
SELECT test_bms_is_subset('(b 1 3 5)', '(b 1 3)') AS result;
SELECT test_bms_is_subset('(b 1 3)', '(b 2 4)') AS result;
SELECT test_bms_is_subset(test_bms_add_range(NULL, 0, 31),
                          test_bms_add_range(NULL, 0, 63)) AS result;
-- Is subset with shorter word counts?
SELECT test_bms_is_subset('(b 5 100)', '(b 5)') AS result;
SELECT test_bms_is_subset('(b 1 2 50 100)', '(b 1 2)') AS result;
-- Test module checks
SELECT test_bms_is_subset('(b 5)', NULL) AS result;
SELECT test_bms_is_subset(NULL, '(b 5)') AS result;
SELECT test_bms_is_subset(NULL, NULL) AS result;

-- bms_subset_compare()
SELECT test_bms_subset_compare(NULL, NULL) AS result;
SELECT test_bms_subset_compare(NULL, '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b)', '(b)') AS result;
SELECT test_bms_subset_compare('(b)', '(b 1)') AS result;
SELECT test_bms_subset_compare('(b 1)', '(b)') AS result;
SELECT test_bms_subset_compare('(b 1 3)', NULL) AS result;
SELECT test_bms_subset_compare('(b 1 3 5)', '(b 1 3 5)') AS result;
SELECT test_bms_subset_compare('(b 1 3)', '(b 1 3 5)') AS result;
SELECT test_bms_subset_compare('(b 1 3 5)', '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b 1 2)', '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b 1 2)', '(b 1 4)') AS result;
SELECT test_bms_subset_compare('(b 1 3)', '(b 1 3 64)') AS result;
SELECT test_bms_subset_compare('(b 1 3 64)', '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b 1 3 64)', '(b 1 3 65)') AS result;
SELECT test_bms_subset_compare('(b 1 3)', '(b 2 4)') AS result;
SELECT test_bms_subset_compare('(b 1)', '(b 64)') AS result;
SELECT test_bms_subset_compare('(b 0)', '(b 32)') AS result;
SELECT test_bms_subset_compare('(b 0)', '(b 64)') AS result;
SELECT test_bms_subset_compare('(b 64)', '(b 1)') AS result;
SELECT test_bms_subset_compare('(b 1 2)', '(b 1 2 64)') AS result;
SELECT test_bms_subset_compare('(b 64 200)', '(b 1 201)') AS result;
SELECT test_bms_subset_compare('(b 1 64 65)', '(b 1 2 64)') AS result;
SELECT test_bms_subset_compare('(b 2 64 128)', '(b 1 65)') AS result;
-- Test module checks
SELECT test_bms_subset_compare('(b 5)', NULL) AS result;
SELECT test_bms_subset_compare(NULL, '(b 5)') AS result;
SELECT test_bms_subset_compare(NULL, NULL) AS result;

-- bms_copy()
SELECT test_bms_copy(NULL) AS result;
SELECT test_bms_copy('(b 1 3 5 7)') AS result;
-- Test module check
SELECT test_bms_copy(NULL) AS result;

-- bms_add_members()
SELECT test_bms_add_members('(b 1 3)', '(b 5 7)') AS result;
SELECT test_bms_add_members('(b 1 3 5)', '(b 2 5 7)') AS result;
SELECT test_bms_add_members('(b 1 3 5)', '(b 100 200 300)') AS result;

-- bitmap_hash()
SELECT test_bitmap_hash('(b)') = 0 AS result;
SELECT test_bitmap_hash('(b 1 3 5)') = test_bitmap_hash('(b 1 3 5)') AS result;
SELECT test_bitmap_hash('(b 1 3 5)') = test_bms_hash_value('(b 1 3 5)') AS result;
SELECT test_bitmap_hash('(b 1 3 5)') != test_bitmap_hash('(b 2 4 6)') AS result;
-- Test module check
SELECT test_bitmap_hash(NULL) AS result;

-- bitmap_match()
SELECT test_bitmap_match('(b)', '(b)') AS result;
SELECT test_bitmap_match('(b)', '(b 1 3 5)') AS result;
SELECT test_bitmap_match('(b 1 3 5)', '(b)') AS result;
SELECT test_bitmap_match('(b 1 3 5)', '(b 1 3 5)') AS result;
SELECT test_bitmap_match('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bitmap_match('(b 1 3)', '(b 1 3 5)') AS result;
-- Check relationship of bitmap_match() with bms_equal()
SELECT (test_bitmap_match('(b 1 3 5)', '(b 1 3 5)') = 0) =
        test_bms_equal('(b 1 3 5)', '(b 1 3 5)') AS result;
SELECT (test_bitmap_match('(b 1 3 5)', '(b 2 4 6)') = 0) =
        test_bms_equal('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT (test_bitmap_match('(b)', '(b)') = 0) =
        test_bms_equal('(b)', '(b)') AS result;
-- Test module checks
SELECT test_bitmap_match('(b 5)', NULL) AS result;
SELECT test_bitmap_match(NULL, '(b 5)') AS result;
SELECT test_bitmap_match(NULL, NULL) AS result;

-- bms_overlap_list()
SELECT test_bms_overlap_list('(b 0)', ARRAY[0]) AS result;
SELECT test_bms_overlap_list('(b 2 3)', ARRAY[1,2]) AS result;
SELECT test_bms_overlap_list('(b 3 4)', ARRAY[3,4,5]) AS result;
SELECT test_bms_overlap_list('(b 7 10)', ARRAY[6,7,8,9]) AS result;
SELECT test_bms_overlap_list('(b 1 5)', ARRAY[6,7,8,9]) AS result;
-- Empty list
SELECT test_bms_overlap_list('(b 1)', ARRAY[]::integer[]) AS result;
-- Overlap list with negative numbers
SELECT test_bms_overlap_list('(b 5 10)', ARRAY[-1,5]) AS result; -- error
SELECT test_bms_overlap_list('(b 1 2 3)', ARRAY[-5,-1,0]) AS result; -- error
-- Test module checks
SELECT test_bms_overlap_list('(b 5)', NULL) AS result;
SELECT test_bms_overlap_list(NULL, ARRAY[1,2,3]) AS result;
SELECT test_bms_overlap_list(NULL, NULL) AS result;

-- bms_nonempty_difference()
SELECT test_bms_nonempty_difference(NULL, '(b 1 3 5)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', NULL) AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 1 5)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 1 3 5)') AS result;
-- Difference with different word counts
SELECT test_bms_nonempty_difference('(b 5)', '(b 100)') AS result;
SELECT test_bms_nonempty_difference('(b 100)', '(b 5)') AS result;
SELECT test_bms_nonempty_difference('(b 1 2)', '(b 50 100)') AS result;
-- Test module checks
SELECT test_bms_nonempty_difference('(b 5)', NULL) AS result;
SELECT test_bms_nonempty_difference(NULL, '(b 5)') AS result;
SELECT test_bms_nonempty_difference(NULL, NULL) AS result;

-- random operations
SELECT test_random_operations(-1, 10000, 81920, 0) > 0 AS result;

DROP EXTENSION test_bitmapset;
