/*--------------------------------------------------------------------------
 *
 * test_radixtree.c
 *		Test module for adaptive radix tree.
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
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
				 "%s yielded %" PRIx64 ", expected %" PRIx64 " (%s) in file \"%s\" line %u", \
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


/* define the radix tree implementations to test */
#define RT_PREFIX quart
#define RT_SCOPE
#define RT_DECLARE
#define RT_DEFINE
#define RT_USE_QUART
#define RT_VALUE_TYPE TestValueType
#ifdef TEST_SHARED_RT
#define RT_SHMEM
#endif
#define RT_DEBUG
#include "lib/radixtree.h"

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

static void test_quart(void);
static void test_quart_bulk(void);

static void
test_empty(void)
{
	rt_radix_tree *radixtree;
	rt_iter    *iter;
	uint64		key;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId("test_radix_tree");
	dsa_area   *dsa;

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
	int			tranche_id = LWLockNewTrancheId("test_radix_tree");
	dsa_area   *dsa;

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

	keys = palloc_array(uint64, children);
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
		EXPECT_TRUE(rt_set(radixtree, keys[i], &update));
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
	int			tranche_id = LWLockNewTrancheId("test_radix_tree");
	dsa_area   *dsa;

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
	test_quart();
	test_quart_bulk();

	PG_RETURN_VOID();
}

/*
 * Test QuART fast-path insertion against plain insertion.  The key stream is
 * near-sorted, starts above 255, has outliers in other groups, and mixes in
 * plain quart_set() calls, all of which used to corrupt the QuART tree.
 */
static void
test_quart(void)
{
	quart_radix_tree *quarttree;
	rt_radix_tree *radixtree;
	quart_iter *iter;
	pg_prng_state state;
	uint64		key;
	TestValueType *val;
	int			num_keys = 100000;
	int64		num_iterated = 0;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId("test_radix_tree");
	dsa_area   *dsa;

	dsa = dsa_create(tranche_id);
	quarttree = quart_create(dsa, tranche_id);
	radixtree = rt_create(dsa, tranche_id);
#else
	MemoryContext radixtree_ctx;

	radixtree_ctx = AllocSetContextCreate(CurrentMemoryContext,
										  "test_radix_tree",
										  ALLOCSET_DEFAULT_SIZES);
	quarttree = quart_create(radixtree_ctx);
	radixtree = rt_create(radixtree_ctx);
#endif

	pg_prng_seed(&state, 0);
	for (int i = 0; i < num_keys; i++)
	{
		TestValueType v;

		if (pg_prng_uint32(&state) % 100 < 5)
			key = pg_prng_uint32(&state) % (1 << 24);	/* outlier */
		else
			key = 1000 + i;
		v = (TestValueType) key;

		if (i % 7 == 3)
			quart_set(quarttree, key, &v);
		else
			quart_set_quart(quarttree, key, &v);
		rt_set(radixtree, key, &v);
	}

	/* the QuART tree must hold exactly the keys of the plain tree */
	EXPECT_EQ_U64(quarttree->ctl->num_keys, rt_num_entries(radixtree));

	iter = quart_begin_iterate(quarttree);
	while ((val = quart_iterate_next(iter, &key)) != NULL)
	{
		EXPECT_EQ_U64(*val, key);
		EXPECT_TRUE(rt_find(radixtree, key) != NULL);
		num_iterated++;
	}
	quart_end_iterate(iter);
	EXPECT_EQ_U64(num_iterated, rt_num_entries(radixtree));

	quart_free(quarttree);
	rt_free(radixtree);

#ifdef TEST_SHARED_RT
	dsa_detach(dsa);
#endif
}

/*
 * Build a QuART tree by inserting "pre" with quart_set_quart(), bulk-loading
 * "keys" (strictly ascending) and inserting "post" with quart_set_quart(), and
 * check that it holds exactly what plain rt_set() calls produce.
 */
static void
test_quart_bulk_case(uint64 *pre, int npre, uint64 *keys, int nkeys,
					 uint64 *post, int npost)
{
	quart_radix_tree *quarttree;
	rt_radix_tree *radixtree;
	quart_iter *iter;
	TestValueType *values;
	TestValueType *val;
	uint64		key;
	int64		num_iterated = 0;
#ifdef TEST_SHARED_RT
	int			tranche_id = LWLockNewTrancheId("test_radix_tree");
	dsa_area   *dsa;

	dsa = dsa_create(tranche_id);
	quarttree = quart_create(dsa, tranche_id);
	radixtree = rt_create(dsa, tranche_id);
#else
	MemoryContext radixtree_ctx;

	radixtree_ctx = AllocSetContextCreate(CurrentMemoryContext,
										  "test_radix_tree",
										  ALLOCSET_DEFAULT_SIZES);
	quarttree = quart_create(radixtree_ctx);
	radixtree = rt_create(radixtree_ctx);
#endif

	for (int i = 0; i < npre; i++)
	{
		quart_set_quart(quarttree, pre[i], (TestValueType *) &pre[i]);
		rt_set(radixtree, pre[i], (TestValueType *) &pre[i]);
	}

	values = palloc(sizeof(TestValueType) * Max(nkeys, 1));
	for (int i = 0; i < nkeys; i++)
	{
		values[i] = (TestValueType) keys[i];
		rt_set(radixtree, keys[i], &values[i]);
	}
	quart_set_quart_bulk(quarttree, keys, values, nkeys);
	pfree(values);

	for (int i = 0; i < npost; i++)
	{
		quart_set_quart(quarttree, post[i], (TestValueType *) &post[i]);
		rt_set(radixtree, post[i], (TestValueType *) &post[i]);
	}

	EXPECT_EQ_U64(quarttree->ctl->num_keys, rt_num_entries(radixtree));

	iter = quart_begin_iterate(quarttree);
	while ((val = quart_iterate_next(iter, &key)) != NULL)
	{
		EXPECT_EQ_U64(*val, key);
		EXPECT_TRUE(rt_find(radixtree, key) != NULL);
		num_iterated++;
	}
	quart_end_iterate(iter);
	EXPECT_EQ_U64(num_iterated, rt_num_entries(radixtree));

	quart_free(quarttree);
	rt_free(radixtree);

#ifdef TEST_SHARED_RT
	dsa_detach(dsa);
#endif
}

/*
 * Test grouped bulk loading on dense, gapped and offset sorted streams, whose
 * groups cover every node size class, and its interaction with the stail fast
 * path on a non-empty tree.
 */
static void
test_quart_bulk(void)
{
	/* max gap between consecutive keys; 1 is dense */
	static const int max_gaps[] = {1, 2, 3, 6, 9, 20, 300, 100000};
	int			num_keys = 50000;
	uint64	   *keys = palloc(sizeof(uint64) * num_keys);
	uint64	   *other = palloc(sizeof(uint64) * num_keys);
	pg_prng_state state;
	uint64		key;
	int			n;

	pg_prng_seed(&state, 0);

	for (int g = 0; g < lengthof(max_gaps); g++)
	{
		key = 1000;
		for (int i = 0; i < num_keys; i++)
		{
			keys[i] = key;
			key += 1 + pg_prng_uint64_range(&state, 0, max_gaps[g] - 1);
		}
		test_quart_bulk_case(NULL, 0, keys, num_keys, NULL, 0);
	}

	/* dense bands separated by gaps, starting at a 64-bit offset */
	key = UINT64CONST(0x7FFFFFFF00000000);
	n = 0;
	while (n < num_keys)
	{
		int			band = 1 + pg_prng_uint64_range(&state, 0, 1000);

		for (int i = 0; i < band && n < num_keys; i++)
			keys[n++] = key++;
		key += pg_prng_uint64_range(&state, 1, 5000);
	}
	test_quart_bulk_case(NULL, 0, keys, num_keys, NULL, 0);

	/* the highest keys, a lone key, keys that fit in the root, no keys */
	for (int i = 0; i < 300; i++)
		keys[i] = PG_UINT64_MAX - 299 + i;
	test_quart_bulk_case(NULL, 0, keys, 300, NULL, 0);
	keys[0] = 123456789;
	test_quart_bulk_case(NULL, 0, keys, 1, NULL, 0);
	for (int i = 0; i < 100; i++)
		keys[i] = 2 * i;
	test_quart_bulk_case(NULL, 0, keys, 100, NULL, 0);
	test_quart_bulk_case(NULL, 0, keys, 0, NULL, 0);

	/*
	 * Non-empty tree: existing keys, some in the bulk-loaded groups and some
	 * below 256 so that the bulk load extends the tree upward; then stail
	 * inserts into the bulk-loaded groups.
	 */
	for (int i = 0; i < num_keys; i++)
		keys[i] = 100000 + 3 * i;
	for (int i = 0; i < 1000; i++)
		other[i] = pg_prng_uint64_range(&state, 0, 300000);
	other[1000] = 7;
	test_quart_bulk_case(other + 1000, 1, keys, num_keys, other, 1001);
	test_quart_bulk_case(other, 1001, keys, num_keys, NULL, 0);
	for (int i = 0; i < num_keys; i++)
		other[i] = keys[i] + 1;
	test_quart_bulk_case(NULL, 0, keys, num_keys, other, num_keys);
	test_quart_bulk_case(keys + 1, num_keys - 1, keys, num_keys, NULL, 0);

	pfree(keys);
	pfree(other);
}
