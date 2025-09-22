-- Tests for Bitmapsets
CREATE EXTENSION test_bitmapset;

-- bms_make_singleton()
SELECT test_bms_make_singleton(-1);
SELECT test_bms_make_singleton(42) AS result;
SELECT test_bms_make_singleton(0) AS result;
SELECT test_bms_make_singleton(1000) AS result;

-- bms_add_member()
SELECT test_bms_add_member('(b 1)', -1); -- error
SELECT test_bms_add_member('(b)', -10); -- error
SELECT test_bms_add_member('(b)', 10) AS result;
SELECT test_bms_add_member('(b 5)', 10) AS result;
-- sort check
SELECT test_bms_add_member('(b 10)', 5) AS result;
-- idempotent change
SELECT test_bms_add_member('(b 10)', 10) AS result;

-- bms_replace_members()
SELECT test_bms_replace_members(NULL, '(b 1 2 3)') AS result;
SELECT test_bms_replace_members('(b 1 2 3)', NULL) AS result;
SELECT test_bms_replace_members('(b 1 2 3)', '(b 3 5 6)') AS result;
SELECT test_bms_replace_members('(b 1 2 3)', '(b 3 5)') AS result;
SELECT test_bms_replace_members('(b 1 2)', '(b 3 5 7)') AS result;

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

-- bms_join()
SELECT test_bms_join('(b 1 3 5)', NULL) AS result;
SELECT test_bms_join(NULL, '(b 2 4 6)') AS result;
SELECT test_bms_join('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_join('(b 1 3 5)', '(b 1 4 5)') AS result;

-- bms_union()
-- Overlapping sets.
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

-- bms_intersect()
-- Overlapping sets
SELECT test_bms_intersect('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Disjoint sets
SELECT test_bms_intersect('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Intersect with empty.
SELECT test_bms_intersect('(b 1 3 5)', '(b)') AS result;

-- bms_int_members()
-- Overlapping sets
SELECT test_bms_int_members('(b 1 3 5)', '(b 3 5 7)') AS result;
-- Disjoint sets
SELECT test_bms_int_members('(b 1 3 5)', '(b 2 4 6)') AS result;
-- Intersect with empty.
SELECT test_bms_int_members('(b 1 3 5)', '(b)') AS result;
-- Multiple members
SELECT test_bms_int_members('(b 0 31 32 63 64)', '(b 31 32 64 65)') AS result;

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

-- bms_is_member()
SELECT test_bms_is_member('(b)', -5); -- error
SELECT test_bms_is_member('(b 1 3 5)', 1) AS result;
SELECT test_bms_is_member('(b 1 3 5)', 2) AS result;
SELECT test_bms_is_member('(b 1 3 5)', 3) AS result;
SELECT test_bms_is_member('(b)', 1) AS result;

-- bms_member_index()
SELECT test_bms_member_index(NULL, 1) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 2) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 1) AS result;
SELECT test_bms_member_index('(b 1 3 5)', 3) AS result;

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

-- bms_membership()
SELECT test_bms_membership('(b)') AS result;
SELECT test_bms_membership('(b 42)') AS result;
SELECT test_bms_membership('(b 1 2)') AS result;

-- bms_is_empty()
SELECT test_bms_is_empty(NULL) AS result;
SELECT test_bms_is_empty('(b)') AS result;
SELECT test_bms_is_empty('(b 1)') AS result;

-- bms_singleton_member()
SELECT test_bms_singleton_member('(b 1 2)'); -- error
SELECT test_bms_singleton_member('(b 42)') AS result;

-- bms_get_singleton_member()
-- Not a singleton, returns input default
SELECT test_bms_get_singleton_member('(b 3 6)', 1000) AS result;
-- Singletone, returns sole member
SELECT test_bms_get_singleton_member('(b 400)', 1000) AS result;

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

-- bms_hash_value()
SELECT test_bms_hash_value('(b)') = 0 AS result;
SELECT test_bms_hash_value('(b 1 3 5)') = test_bms_hash_value('(b 1 3 5)') AS result;
SELECT test_bms_hash_value('(b 1 3 5)') != test_bms_hash_value('(b 2 4 6)') AS result;

-- bms_overlap()
SELECT test_bms_overlap('(b 1 3 5)', '(b 3 5 7)') AS result;
SELECT test_bms_overlap('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_overlap('(b)', '(b 1 3 5)') AS result;

-- bms_is_subset()
SELECT test_bms_is_subset('(b)', '(b 1 3 5)') AS result;
SELECT test_bms_is_subset('(b 1 3)', '(b 1 3 5)') AS result;
SELECT test_bms_is_subset('(b 1 3 5)', '(b 1 3)') AS result;
SELECT test_bms_is_subset('(b 1 3)', '(b 2 4)') AS result;
SELECT test_bms_is_subset(test_bms_add_range(NULL, 0, 31),
                          test_bms_add_range(NULL, 0, 63)) AS result;

-- bms_subset_compare()
SELECT test_bms_subset_compare(NULL, NULL) AS result;
SELECT test_bms_subset_compare('(b 1 3)', NULL) AS result;
SELECT test_bms_subset_compare(NULL, '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b 1 3 5)', '(b 1 3)') AS result;
SELECT test_bms_subset_compare('(b 1 3)', '(b 1 3 5)') AS result;
SELECT test_bms_subset_compare('(b 1 3 5)', '(b 1 3 5)') AS result;
SELECT test_bms_subset_compare('(b 1 3 5)', '(b 2 4 6)') AS result;

-- bms_copy()
SELECT test_bms_copy(NULL) AS result;
SELECT test_bms_copy('(b 1 3 5 7)') AS result;

-- bms_add_members()
SELECT test_bms_add_member('(b)', 1000); -- error
SELECT test_bms_add_members('(b 1 3)', '(b 5 7)') AS result;
SELECT test_bms_add_members('(b 1 3 5)', '(b 2 5 7)') AS result;
SELECT test_bms_add_members('(b 1 3 5)', '(b 100 200 300)') AS result;

-- bitmap_hash()
SELECT test_bitmap_hash('(b)') = 0 AS result;
SELECT test_bitmap_hash('(b 1 3 5)') = test_bitmap_hash('(b 1 3 5)') AS result;
SELECT test_bitmap_hash('(b 1 3 5)') = test_bms_hash_value('(b 1 3 5)') AS result;
SELECT test_bitmap_hash('(b 1 3 5)') != test_bitmap_hash('(b 2 4 6)') AS result;

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

-- bms_overlap_list()
SELECT test_bms_overlap_list('(b 0)', ARRAY[0]) AS result;
SELECT test_bms_overlap_list('(b 2 3)', ARRAY[1,2]) AS result;
SELECT test_bms_overlap_list('(b 3 4)', ARRAY[3,4,5]) AS result;
SELECT test_bms_overlap_list('(b 7 10)', ARRAY[6,7,8,9]) AS result;
SELECT test_bms_overlap_list('(b 1 5)', ARRAY[6,7,8,9]) AS result;
-- Empty list
SELECT test_bms_overlap_list('(b 1)', ARRAY[]::integer[]) AS result;

-- bms_nonempty_difference()
SELECT test_bms_nonempty_difference(NULL, '(b 1 3 5)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', NULL) AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 2 4 6)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 1 5)') AS result;
SELECT test_bms_nonempty_difference('(b 1 3 5)', '(b 1 3 5)') AS result;

-- random operations
SELECT test_random_operations(-1, 10000, 81920, 0) > 0 AS result;

DROP EXTENSION test_bitmapset;
