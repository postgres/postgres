/* src/test/modules/test_bitmapset/test_bitmapset--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_bitmapset" to load this file. \quit

-- Bitmapset API functions
CREATE FUNCTION test_bms_make_singleton(integer)
RETURNS text STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_add_member(text, integer)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_del_member(text, integer)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_is_member(text, integer)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_num_members(text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_copy(text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_equal(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_compare(text, text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_is_subset(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_subset_compare(text, text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_union(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_intersect(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_difference(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_is_empty(text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_membership(text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_singleton_member(text)
RETURNS integer STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_get_singleton_member(text, integer)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_next_member(text, integer)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_prev_member(text, integer)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_hash_value(text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_overlap(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_overlap_list(text, int4[])
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_nonempty_difference(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_member_index(text, integer)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_add_range(text, integer, integer)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_add_members(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_int_members(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_del_members(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_replace_members(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bms_join(text, text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bitmap_hash(text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bitmap_match(text, text)
RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Test utility functions
CREATE FUNCTION test_random_operations(integer, integer, integer, integer)
RETURNS integer STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON EXTENSION test_bitmapset IS 'Test code for Bitmapset';
