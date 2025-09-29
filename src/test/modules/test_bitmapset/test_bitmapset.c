/*-------------------------------------------------------------------------
 *
 * test_bitmapset.c
 *      Test the Bitmapset data structure.
 *
 * This module tests the Bitmapset implementation in PostgreSQL, covering
 * all public API functions.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_bitmapset/test_bitmapset.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stddef.h>
#include "catalog/pg_type.h"
#include "common/pg_prng.h"
#include "utils/array.h"
#include "fmgr.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/* Bitmapset API functions in order of appearance in bitmapset.c */
PG_FUNCTION_INFO_V1(test_bms_make_singleton);
PG_FUNCTION_INFO_V1(test_bms_add_member);
PG_FUNCTION_INFO_V1(test_bms_del_member);
PG_FUNCTION_INFO_V1(test_bms_is_member);
PG_FUNCTION_INFO_V1(test_bms_num_members);
PG_FUNCTION_INFO_V1(test_bms_copy);
PG_FUNCTION_INFO_V1(test_bms_equal);
PG_FUNCTION_INFO_V1(test_bms_compare);
PG_FUNCTION_INFO_V1(test_bms_is_subset);
PG_FUNCTION_INFO_V1(test_bms_subset_compare);
PG_FUNCTION_INFO_V1(test_bms_union);
PG_FUNCTION_INFO_V1(test_bms_intersect);
PG_FUNCTION_INFO_V1(test_bms_difference);
PG_FUNCTION_INFO_V1(test_bms_is_empty);
PG_FUNCTION_INFO_V1(test_bms_membership);
PG_FUNCTION_INFO_V1(test_bms_singleton_member);
PG_FUNCTION_INFO_V1(test_bms_get_singleton_member);
PG_FUNCTION_INFO_V1(test_bms_next_member);
PG_FUNCTION_INFO_V1(test_bms_prev_member);
PG_FUNCTION_INFO_V1(test_bms_hash_value);
PG_FUNCTION_INFO_V1(test_bms_overlap);
PG_FUNCTION_INFO_V1(test_bms_overlap_list);
PG_FUNCTION_INFO_V1(test_bms_nonempty_difference);
PG_FUNCTION_INFO_V1(test_bms_member_index);
PG_FUNCTION_INFO_V1(test_bms_add_range);
PG_FUNCTION_INFO_V1(test_bms_add_members);
PG_FUNCTION_INFO_V1(test_bms_int_members);
PG_FUNCTION_INFO_V1(test_bms_del_members);
PG_FUNCTION_INFO_V1(test_bms_replace_members);
PG_FUNCTION_INFO_V1(test_bms_join);
PG_FUNCTION_INFO_V1(test_bitmap_hash);
PG_FUNCTION_INFO_V1(test_bitmap_match);

/* Test utility functions */
PG_FUNCTION_INFO_V1(test_random_operations);

/* Convenient macros to test results */
#define EXPECT_TRUE(expr)	\
	do { \
		if (!(expr)) \
			elog(ERROR, \
				 "%s was unexpectedly false in file \"%s\" line %u", \
				 #expr, __FILE__, __LINE__); \
	} while (0)

#define EXPECT_NOT_NULL(expr)	\
	do { \
		if ((expr) == NULL) \
			elog(ERROR, \
				 "%s was unexpectedly true in file \"%s\" line %u", \
				 #expr, __FILE__, __LINE__); \
	} while (0)

/* Encode/Decode to/from TEXT and Bitmapset */
#define BITMAPSET_TO_TEXT(bms) cstring_to_text(nodeToString(bms))
#define TEXT_TO_BITMAPSET(str) ((Bitmapset *) stringToNode(text_to_cstring(str)))

/*
 * Individual test functions for each bitmapset API function
 */

Datum
test_bms_add_member(PG_FUNCTION_ARGS)
{
	int			member;
	Bitmapset  *bms = NULL;
	text	   *result;

	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	member = PG_GETARG_INT32(1);
	bms = bms_add_member(bms, member);
	result = BITMAPSET_TO_TEXT(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_add_members(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	/* IMPORTANT: bms_add_members modifies/frees the first argument */
	bms1 = bms_add_members(bms1, bms2);

	if (bms2)
		bms_free(bms2);

	result = BITMAPSET_TO_TEXT(bms1);
	bms_free(bms1);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_del_member(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int32		member;
	text	   *result;

	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	member = PG_GETARG_INT32(1);
	bms = bms_del_member(bms, member);

	if (bms_is_empty(bms))
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(bms);
	bms_free(bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_is_member(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int32		member;
	bool		result;

	if (PG_ARGISNULL(1))
		PG_RETURN_BOOL(false);

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	member = PG_GETARG_INT32(1);
	result = bms_is_member(member, bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_num_members(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int			result = 0;

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	result = bms_num_members(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32(result);
}

Datum
test_bms_make_singleton(PG_FUNCTION_ARGS)
{
	int32		member;
	Bitmapset  *bms;
	text	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	member = PG_GETARG_INT32(0);
	bms = bms_make_singleton(member);

	result = BITMAPSET_TO_TEXT(bms);
	bms_free(bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_copy(PG_FUNCTION_ARGS)
{
	text	   *bms_data;
	Bitmapset  *bms = NULL;
	Bitmapset  *copy_bms;
	text	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	bms_data = PG_GETARG_TEXT_PP(0);
	bms = TEXT_TO_BITMAPSET(bms_data);
	copy_bms = bms_copy(bms);
	result = BITMAPSET_TO_TEXT(copy_bms);

	if (bms)
		bms_free(bms);
	if (copy_bms)
		bms_free(copy_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_equal(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	bool		result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_equal(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_union(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result_bms = bms_union(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_membership(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	BMS_Membership result;

	if (PG_ARGISNULL(0))
		PG_RETURN_INT32(BMS_EMPTY_SET);

	bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));
	result = bms_membership(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32((int32) result);
}

Datum
test_bms_next_member(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int32		prevmember;
	int			result;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_INT32(-2);

	bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));
	prevmember = PG_GETARG_INT32(1);
	result = bms_next_member(bms, prevmember);

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32(result);
}

Datum
test_bms_intersect(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result_bms = bms_intersect(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_difference(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result_bms = bms_difference(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_compare(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	int			result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_compare(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_INT32(result);
}

Datum
test_bms_is_empty(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	bool		result;

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	result = bms_is_empty(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_is_subset(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	bool		result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_is_subset(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_subset_compare(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	BMS_Comparison result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_subset_compare(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_INT32((int32) result);
}

Datum
test_bms_singleton_member(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int			result;

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	result = bms_singleton_member(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32(result);
}

Datum
test_bms_get_singleton_member(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int32		default_member = PG_GETARG_INT32(1);
	bool		success;
	int			member = -1;

	if (PG_ARGISNULL(0))
		PG_RETURN_INT32(default_member);

	bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	/*
	 * bms_get_singleton_member returns bool and stores result in member
	 * pointer
	 */
	success = bms_get_singleton_member(bms, &member);
	bms_free(bms);

	if (success)
		PG_RETURN_INT32(member);

	PG_RETURN_INT32(default_member);
}

Datum
test_bms_prev_member(PG_FUNCTION_ARGS)
{
	text	   *bms_data;
	Bitmapset  *bms = NULL;
	int32		prevmember;
	int			result;

	if (PG_ARGISNULL(0))
		PG_RETURN_INT32(-2);

	bms_data = PG_GETARG_TEXT_PP(0);
	prevmember = PG_GETARG_INT32(1);

	if (VARSIZE_ANY_EXHDR(bms_data) == 0)
		PG_RETURN_INT32(-2);

	bms = TEXT_TO_BITMAPSET(bms_data);
	result = bms_prev_member(bms, prevmember);
	bms_free(bms);

	PG_RETURN_INT32(result);
}

Datum
test_bms_overlap(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	bool		result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_overlap(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_overlap_list(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	ArrayType  *array;
	List	   *int_list = NIL;
	bool		result;
	Datum	   *elem_datums;
	bool	   *elem_nulls;
	int			elem_count;
	int			i;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);

	bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
	{
		if (bms)
			bms_free(bms);
		PG_RETURN_BOOL(false);
	}

	array = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array(array,
					  INT4OID, sizeof(int32), true, 'i',
					  &elem_datums, &elem_nulls, &elem_count);

	for (i = 0; i < elem_count; i++)
	{
		if (!elem_nulls[i])
		{
			int32		member = DatumGetInt32(elem_datums[i]);

			int_list = lappend_int(int_list, member);
		}
	}

	result = bms_overlap_list(bms, int_list);

	if (bms)
		bms_free(bms);

	list_free(int_list);

	pfree(elem_datums);
	pfree(elem_nulls);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_nonempty_difference(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	bool		result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	result = bms_nonempty_difference(bms1, bms2);

	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_BOOL(result);
}

Datum
test_bms_member_index(PG_FUNCTION_ARGS)
{
	text	   *bms_data;
	Bitmapset  *bms = NULL;
	int32		member;
	int			result;

	if (PG_ARGISNULL(0))
		PG_RETURN_INT32(-1);

	bms_data = PG_GETARG_TEXT_PP(0);
	member = PG_GETARG_INT32(1);

	if (VARSIZE_ANY_EXHDR(bms_data) == 0)
		PG_RETURN_INT32(-1);

	bms = TEXT_TO_BITMAPSET(bms_data);

	result = bms_member_index(bms, member);
	bms_free(bms);

	PG_RETURN_INT32(result);
}

Datum
test_bms_add_range(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	int32		lower,
				upper;
	text	   *result;

	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_NULL();

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	lower = PG_GETARG_INT32(1);
	upper = PG_GETARG_INT32(2);

	/* Check for invalid range */
	if (upper < lower)
	{
		if (bms)
			bms_free(bms);
		PG_RETURN_NULL();
	}

	bms = bms_add_range(bms, lower, upper);

	result = BITMAPSET_TO_TEXT(bms);
	if (bms)
		bms_free(bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_int_members(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	bms1 = bms_int_members(bms1, bms2);

	if (bms2)
		bms_free(bms2);

	if (bms1 == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(bms1);

	if (bms1)
		bms_free(bms1);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_del_members(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	/* IMPORTANT: bms_del_members modifies/frees the first argument */
	result_bms = bms_del_members(bms1, bms2);

	/* bms1 is now invalid, do not free it */

	if (bms2)
		bms_free(bms2);

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_replace_members(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	/* IMPORTANT: bms_replace_members modifies/frees the first argument */
	result_bms = bms_replace_members(bms1, bms2);

	/* bms1 is now invalid, do not free it */

	if (bms2)
		bms_free(bms2);

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_join(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *result_bms;
	text	   *result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	/* IMPORTANT: bms_join may recycle either input arguments */
	result_bms = bms_join(bms1, bms2);

	/* bms1 and bms2 may have been recycled! Do not free any of them. */

	if (result_bms == NULL)
		PG_RETURN_NULL();

	result = BITMAPSET_TO_TEXT(result_bms);
	bms_free(result_bms);

	PG_RETURN_TEXT_P(result);
}

Datum
test_bms_hash_value(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	uint32		hash_result;

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	hash_result = bms_hash_value(bms);

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32(hash_result);
}

Datum
test_bitmap_hash(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms = NULL;
	Bitmapset  *bms_ptr;
	uint32		hash_result;

	if (!PG_ARGISNULL(0))
		bms = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	bms_ptr = bms;

	/* Call bitmap_hash */
	hash_result = bitmap_hash(&bms_ptr, sizeof(Bitmapset *));

	/* Clean up */
	if (!PG_ARGISNULL(0) && bms_ptr)
		bms_free(bms_ptr);

	PG_RETURN_INT32(hash_result);
}

Datum
test_bitmap_match(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL,
			   *bms2 = NULL;
	Bitmapset  *bms_ptr1,
			   *bms_ptr2;
	int			match_result;

	if (!PG_ARGISNULL(0))
		bms1 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		bms2 = TEXT_TO_BITMAPSET(PG_GETARG_TEXT_PP(1));

	/* Set up pointers to the Bitmapsets */
	bms_ptr1 = bms1;
	bms_ptr2 = bms2;

	/* Call bitmap_match with addresses of the Bitmapset pointers */
	match_result = bitmap_match(&bms_ptr1, &bms_ptr2, sizeof(Bitmapset *));

	/* Clean up */
	if (bms1)
		bms_free(bms1);
	if (bms2)
		bms_free(bms2);

	PG_RETURN_INT32(match_result);
}

/*
 * Contrary to all the other functions which are one-one mappings with the
 * equivalent C functions, this stresses Bitmapsets in a random fashion for
 * various operations.
 *
 * "min_value" is the minimal value used for the members, that will stand
 * up to a range of "max_range".  "num_ops" defines the number of time each
 * operation is done.  "seed" is a random seed used to calculate the member
 * values.
 *
 * The return value is the number of times all operations have been executed.
 */
Datum
test_random_operations(PG_FUNCTION_ARGS)
{
	Bitmapset  *bms1 = NULL;
	Bitmapset  *bms2 = NULL;
	Bitmapset  *bms = NULL;
	Bitmapset  *result = NULL;
	pg_prng_state state;
	uint64		seed = GetCurrentTimestamp();
	int			num_ops = 5000;
	int			total_ops = 0;
	int			max_range = 2000;
	int			min_value = 0;
	int			member;
	int		   *members;
	int			num_members = 0;

	if (!PG_ARGISNULL(0) && PG_GETARG_INT32(0) > 0)
		seed = PG_GETARG_INT32(0);

	if (!PG_ARGISNULL(1))
		num_ops = PG_GETARG_INT32(1);

	if (!PG_ARGISNULL(2))
		max_range = PG_GETARG_INT32(2);

	if (!PG_ARGISNULL(3))
		min_value = PG_GETARG_INT32(3);

	pg_prng_seed(&state, seed);
	members = palloc(sizeof(int) * num_ops);

	/* Phase 1: Random insertions */
	for (int i = 0; i < num_ops / 2; i++)
	{
		member = pg_prng_uint32(&state) % max_range + min_value;

		if (!bms_is_member(member, bms1))
		{
			members[num_members++] = member;
			bms1 = bms_add_member(bms1, member);
		}
	}

	/* Phase 2: Random set operations */
	for (int i = 0; i < num_ops / 4; i++)
	{
		member = pg_prng_uint32(&state) % max_range + min_value;

		bms2 = bms_add_member(bms2, member);
	}

	/* Test union */
	result = bms_union(bms1, bms2);
	EXPECT_NOT_NULL(result);

	/* Verify union contains all members from first set */
	for (int i = 0; i < num_members; i++)
	{
		if (!bms_is_member(members[i], result))
			elog(ERROR, "union missing member %d", members[i]);
	}
	bms_free(result);

	/* Test intersection */
	result = bms_intersect(bms1, bms2);
	if (result != NULL)
	{
		member = -1;

		while ((member = bms_next_member(result, member)) >= 0)
		{
			if (!bms_is_member(member, bms1) || !bms_is_member(member, bms2))
				elog(ERROR, "intersection contains invalid member %d", member);
		}
		bms_free(result);
	}

	/* Phase 3: Test range operations */
	result = NULL;
	for (int i = 0; i < num_ops; i++)
	{
		int			lower = pg_prng_uint32(&state) % 100;
		int			upper = lower + (pg_prng_uint32(&state) % 20);

		result = bms_add_range(result, lower, upper);
	}
	if (result != NULL)
	{
		EXPECT_TRUE(bms_num_members(result) > 0);
		bms_free(result);
	}

	pfree(members);
	bms_free(bms1);
	bms_free(bms2);

	for (int i = 0; i < num_ops; i++)
	{
		member = pg_prng_uint32(&state) % max_range + min_value;
		switch (pg_prng_uint32(&state) % 3)
		{
			case 0:				/* add */
				bms = bms_add_member(bms, member);
				break;
			case 1:				/* delete */
				if (bms != NULL)
				{
					bms = bms_del_member(bms, member);
				}
				break;
			case 2:				/* test membership */
				if (bms != NULL)
				{
					bms_is_member(member, bms);
				}
				break;
		}
		total_ops++;
	}

	if (bms)
		bms_free(bms);

	PG_RETURN_INT32(total_ops);
}
