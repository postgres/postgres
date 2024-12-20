/*--------------------------------------------------------------------------
 *
 * test_radixtree.c
 *		Test module for adaptive radix tree.
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_radixtree/test_radixtree.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* uncomment to use shared memory for the tree */
/* #define TEST_SHARED_RT */

/* Convenient macros to test results */
#define EXPECT_TRUE(expr)	\
	do { \
		if (!(expr)) \
			elog(ERROR, \
				 "%s was unexpectedly false in file \"%s\" line %u", \
				 #expr, __FILE__, __LINE__); \
	} while (0)

#define EXPECT_FALSE(expr)	\
	do { \
		if (expr) \
			elog(ERROR, \
				 "%s was unexpectedly true in file \"%s\" line %u", \
				 #expr, __FILE__, __LINE__); \
	} while (0)

#define EXPECT_EQ_U64(result_expr, expected_expr)	\
	do { \
		uint64		_result = (result_expr); \
		uint64		_expected = (expected_expr); \
		if (_result != _expected) \
			elog(ERROR, \
				 "%s yielded " UINT64_HEX_FORMAT ", expected " UINT64_HEX_FORMAT " (%s) in file \"%s\" line %u", \
				 #result_expr, _result, _expected, #expected_expr, __FILE__, __LINE__); \
	} while (0)

/*
 * With uint64, 64-bit platforms store the value in the last-level child
 * pointer, and 32-bit platforms store this in a single-value leaf.
 * This gives us buildfarm coverage for both paths in this module.
 */
typedef uint64 TestValueType;

/*
 * The node class name and the number of keys big enough to grow nodes
 * into each size class.
 */
typedef struct rt_node_class_test_elem
{
	char	   *class_name;
	int			nkeys;
} rt_node_class_test_elem;

static rt_node_class_test_elem rt_node_class_tests[] =
{
	{
		.class_name = "node-4", /* RT_CLASS_4 */
		.nkeys = 2,
	},
	{
		.class_name = "node-16-lo", /* RT_CLASS_16_LO */
		.nkeys = 15,
	},
	{
		.class_name = "node-16-hi", /* RT_CLASS_16_HI */
		.nkeys = 30,
	},
	{
		.class_name = "node-48",	/* RT_CLASS_48 */
		.nkeys = 60,
	},
	{
		.class_name = "node-256",	/* RT_CLASS_256 */
		.nkeys = 256,
	},
};


/* define the radix tree implementation to test */
#define RT_PREFIX rt
#define RT_SCOPE
#define RT_DECLARE
#define RT_DEFINE
#define RT_USE_DELETE
#define RT_VALUE_TYPE TestValueType
#ifdef TEST_SHARED_RT
#define RT_SHMEM
#endif
#define RT_DEBUG
#include "lib/radixtree.h"


/*
 * Return the number of keys in the radix tree.
 */
static uint64
rt_num_entries(rt_radix_tree *tree)
{
	return tree->ctl->num_keys;
}

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_radixtree);

static void
test_empty(void)
{
	rt_radix_tree *radixtree;
	rt_iter    *iter;
	uint64		key;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId();
	dsa_area   *dsa;

	LWLockRegisterTranche(tranche_id, "test_radix_tree");
	dsa = dsa_create(tranche_id);
	radixtree = rt_create(dsa, tranche_id);
#else
	MemoryContext radixtree_ctx;

	radixtree_ctx = AllocSetContextCreate(CurrentMemoryContext,
										  "test_radix_tree",
										  ALLOCSET_SMALL_SIZES);
	radixtree = rt_create(radixtree_ctx);
#endif

	/* Should not find anything in an empty tree */
	EXPECT_TRUE(rt_find(radixtree, 0) == NULL);
	EXPECT_TRUE(rt_find(radixtree, 1) == NULL);
	EXPECT_TRUE(rt_find(radixtree, PG_UINT64_MAX) == NULL);
	EXPECT_FALSE(rt_delete(radixtree, 0));
	EXPECT_TRUE(rt_num_entries(radixtree) == 0);

	/* Iterating on an empty tree should not return anything */
	iter = rt_begin_iterate(radixtree);
	EXPECT_TRUE(rt_iterate_next(iter, &key) == NULL);
	rt_end_iterate(iter);

	rt_free(radixtree);

#ifdef TEST_SHARED_RT
	dsa_detach(dsa);
#endif
}

/* Basic set, find, and delete tests */
static void
test_basic(rt_node_class_test_elem *test_info, int shift, bool asc)
{
	rt_radix_tree *radixtree;
	rt_iter    *iter;
	uint64	   *keys;
	int			children = test_info->nkeys;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId();
	dsa_area   *dsa;

	LWLockRegisterTranche(tranche_id, "test_radix_tree");
	dsa = dsa_create(tranche_id);
	radixtree = rt_create(dsa, tranche_id);
#else
	MemoryContext radixtree_ctx;

	radixtree_ctx = AllocSetContextCreate(CurrentMemoryContext,
										  "test_radix_tree",
										  ALLOCSET_SMALL_SIZES);
	radixtree = rt_create(radixtree_ctx);
#endif

	elog(NOTICE, "testing node %s with shift %d and %s keys",
		 test_info->class_name, shift, asc ? "ascending" : "descending");

	keys = palloc(sizeof(uint64) * children);
	for (int i = 0; i < children; i++)
	{
		if (asc)
			keys[i] = (uint64) i << shift;
		else
			keys[i] = (uint64) (children - 1 - i) << shift;
	}

	/*
	 * Insert keys. Since the tree was just created, rt_set should return
	 * false.
	 */
	for (int i = 0; i < children; i++)
		EXPECT_FALSE(rt_set(radixtree, keys[i], (TestValueType *) &keys[i]));

	rt_stats(radixtree);

	/* look up keys */
	for (int i = 0; i < children; i++)
	{
		TestValueType *value;

		value = rt_find(radixtree, keys[i]);

		/* Test rt_find returns the expected value */
		EXPECT_TRUE(value != NULL);
		EXPECT_EQ_U64(*value, (TestValueType) keys[i]);
	}

	/* update keys */
	for (int i = 0; i < children; i++)
	{
		TestValueType update = keys[i] + 1;

		/* rt_set should report the key found */
		EXPECT_TRUE(rt_set(radixtree, keys[i], (TestValueType *) &update));
	}

	/* delete and re-insert keys */
	for (int i = 0; i < children; i++)
	{
		EXPECT_TRUE(rt_delete(radixtree, keys[i]));
		EXPECT_FALSE(rt_set(radixtree, keys[i], (TestValueType *) &keys[i]));
	}

	/* look up keys after deleting and re-inserting */
	for (int i = 0; i < children; i++)
	{
		TestValueType *value;

		value = rt_find(radixtree, keys[i]);

		/* Test that rt_find returns the expected value */
		EXPECT_TRUE(value != NULL);
		EXPECT_EQ_U64(*value, (TestValueType) keys[i]);
	}

	/* test that iteration returns the expected keys and values */
	iter = rt_begin_iterate(radixtree);

	for (int i = 0; i < children; i++)
	{
		uint64		expected;
		uint64		iterkey;
		TestValueType *iterval;

		/* iteration is ordered by key, so adjust expected value accordingly */
		if (asc)
			expected = keys[i];
		else
			expected = keys[children - 1 - i];

		iterval = rt_iterate_next(iter, &iterkey);

		EXPECT_TRUE(iterval != NULL);
		EXPECT_EQ_U64(iterkey, expected);
		EXPECT_EQ_U64(*iterval, expected);
	}

	rt_end_iterate(iter);

	/* delete all keys again */
	for (int i = 0; i < children; i++)
		EXPECT_TRUE(rt_delete(radixtree, keys[i]));

	/* test that all keys were deleted */
	for (int i = 0; i < children; i++)
		EXPECT_TRUE(rt_find(radixtree, keys[i]) == NULL);

	rt_stats(radixtree);

	pfree(keys);
	rt_free(radixtree);

#ifdef TEST_SHARED_RT
	dsa_detach(dsa);
#endif
}

static int
key_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(*(const uint64 *) a, *(const uint64 *) b);
}

static void
test_random(void)
{
	rt_radix_tree *radixtree;
	rt_iter    *iter;
	pg_prng_state state;

	/* limit memory usage by limiting the key space */
	uint64		filter = ((uint64) (0x07 << 24) | (0xFF << 16) | 0xFF);
	uint64		seed = GetCurrentTimestamp();
	int			num_keys = 100000;
	uint64	   *keys;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId();
	dsa_area   *dsa;

	LWLockRegisterTranche(tranche_id, "test_radix_tree");
	dsa = dsa_create(tranche_id);
	radixtree = rt_create(dsa, tranche_id);
#else
	MemoryContext radixtree_ctx;

	radixtree_ctx = SlabContextCreate(CurrentMemoryContext,
									  "test_radix_tree",
									  SLAB_DEFAULT_BLOCK_SIZE,
									  sizeof(TestValueType));
	radixtree = rt_create(radixtree_ctx);
#endif

	/* add some random values */
	pg_prng_seed(&state, seed);
	keys = (TestValueType *) palloc(sizeof(uint64) * num_keys);
	for (uint64 i = 0; i < num_keys; i++)
	{
		uint64		key = pg_prng_uint64(&state) & filter;
		TestValueType val = (TestValueType) key;

		/* save in an array */
		keys[i] = key;

		rt_set(radixtree, key, &val);
	}

	rt_stats(radixtree);

	for (uint64 i = 0; i < num_keys; i++)
	{
		TestValueType *value;

		value = rt_find(radixtree, keys[i]);

		/* Test rt_find for values just inserted */
		EXPECT_TRUE(value != NULL);
		EXPECT_EQ_U64(*value, keys[i]);
	}

	/* sort keys for iteration and absence tests */
	qsort(keys, num_keys, sizeof(uint64), key_cmp);

	/* should not find numbers in between the keys */
	for (uint64 i = 0; i < num_keys - 1; i++)
	{
		TestValueType *value;

		/* skip duplicate and adjacent keys */
		if (keys[i + 1] == keys[i] || keys[i + 1] == keys[i] + 1)
			continue;

		/* should not find the number right after key */
		value = rt_find(radixtree, keys[i] + 1);
		EXPECT_TRUE(value == NULL);
	}

	/* should not find numbers lower than lowest key */
	for (uint64 key = 0; key < keys[0]; key++)
	{
		TestValueType *value;

		/* arbitrary stopping point */
		if (key > 10000)
			break;

		value = rt_find(radixtree, key);
		EXPECT_TRUE(value == NULL);
	}

	/* should not find numbers higher than highest key */
	for (uint64 i = 1; i < 10000; i++)
	{
		TestValueType *value;

		value = rt_find(radixtree, keys[num_keys - 1] + i);
		EXPECT_TRUE(value == NULL);
	}

	/* test that iteration returns the expected keys and values */
	iter = rt_begin_iterate(radixtree);

	for (int i = 0; i < num_keys; i++)
	{
		uint64		expected;
		uint64		iterkey;
		TestValueType *iterval;

		/* skip duplicate keys */
		if (i < num_keys - 1 && keys[i + 1] == keys[i])
			continue;

		expected = keys[i];
		iterval = rt_iterate_next(iter, &iterkey);

		EXPECT_TRUE(iterval != NULL);
		EXPECT_EQ_U64(iterkey, expected);
		EXPECT_EQ_U64(*iterval, expected);
	}

	rt_end_iterate(iter);

	/* reset random number generator for deletion */
	pg_prng_seed(&state, seed);

	/* delete in original random order */
	for (uint64 i = 0; i < num_keys; i++)
	{
		uint64		key = pg_prng_uint64(&state) & filter;

		rt_delete(radixtree, key);
	}

	EXPECT_TRUE(rt_num_entries(radixtree) == 0);

	pfree(keys);
	rt_free(radixtree);

#ifdef TEST_SHARED_RT
	dsa_detach(dsa);
#endif
}

Datum
test_radixtree(PG_FUNCTION_ARGS)
{
	/* borrowed from RT_MAX_SHIFT */
	const int	max_shift = (sizeof(uint64) - 1) * BITS_PER_BYTE;

	test_empty();

	for (int i = 0; i < lengthof(rt_node_class_tests); i++)
	{
		rt_node_class_test_elem *test_info = &(rt_node_class_tests[i]);

		/* a tree with one level, i.e. a single node under the root node */
		test_basic(test_info, 0, true);
		test_basic(test_info, 0, false);

		/* a tree with two levels */
		test_basic(test_info, 8, true);
		test_basic(test_info, 8, false);

		/* a tree with the maximum number of levels */
		test_basic(test_info, max_shift, true);
		test_basic(test_info, max_shift, false);
	}

	test_random();

	PG_RETURN_VOID();
}
