/*-------------------------------------------------------------------------
 *
 * integerset.c
 *	  Data structure to hold a large set of 64-bit integers efficiently
 *
 * IntegerSet provides an in-memory data structure to hold a set of
 * arbitrary 64-bit integers.  Internally, the values are stored in a
 * B-tree, with a special packed representation at the leaf level using
 * the Simple-8b algorithm, which can pack clusters of nearby values
 * very tightly.
 *
 * Memory consumption depends on the number of values stored, but also
 * on how far the values are from each other.  In the best case, with
 * long runs of consecutive integers, memory consumption can be as low as
 * 0.1 bytes per integer.  In the worst case, if integers are more than
 * 2^32 apart, it uses about 8 bytes per integer.  In typical use, the
 * consumption per integer is somewhere between those extremes, depending
 * on the range of integers stored, and how "clustered" they are.
 *
 *
 * Interface
 * ---------
 *
 *	intset_create			- Create a new, empty set
 *	intset_add_member		- Add an integer to the set
 *	intset_is_member		- Test if an integer is in the set
 *	intset_begin_iterate	- Begin iterating through all integers in set
 *	intset_iterate_next		- Return next set member, if any
 *
 * intset_create() creates the set in the current memory context.  Subsequent
 * operations that add to the data structure will continue to allocate from
 * that same context, even if it's not current anymore.
 *
 * Note that there is no function to free an integer set.  If you need to do
 * that, create a dedicated memory context to hold it, and destroy the memory
 * context instead.
 *
 *
 * Limitations
 * -----------
 *
 * - Values must be added in order.  (Random insertions would require
 *   splitting nodes, which hasn't been implemented.)
 *
 * - Values cannot be added while iteration is in progress.
 *
 * - No support for removing values.
 *
 * None of these limitations are fundamental to the data structure, so they
 * could be lifted if needed, by writing some new code.  But the current
 * users of this facility don't need them.
 *
 *
 * References
 * ----------
 *
 * Simple-8b encoding is based on:
 *
 * Vo Ngoc Anh, Alistair Moffat, Index compression using 64-bit words,
 *   Software - Practice & Experience, v.40 n.2, p.131-147, February 2010
 *   (https://doi.org/10.1002/spe.948)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/lib/integerset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "lib/integerset.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"


/*
 * Maximum number of integers that can be encoded in a single Simple-8b
 * codeword. (Defined here before anything else, so that we can size arrays
 * using this.)
 */
#define SIMPLE8B_MAX_VALUES_PER_CODEWORD 240

/*
 * Parameters for shape of the in-memory B-tree.
 *
 * These set the size of each internal and leaf node.  They don't necessarily
 * need to be the same, because the tree is just an in-memory structure.
 * With the default 64, each node is about 1 kb.
 *
 * If you change these, you must recalculate MAX_TREE_LEVELS, too!
 */
#define MAX_INTERNAL_ITEMS	64
#define MAX_LEAF_ITEMS	64

/*
 * Maximum height of the tree.
 *
 * MAX_TREE_ITEMS is calculated from the "fan-out" of the B-tree.  The
 * theoretical maximum number of items that we can store in a set is 2^64,
 * so MAX_TREE_LEVELS should be set so that:
 *
 *   MAX_LEAF_ITEMS * MAX_INTERNAL_ITEMS ^ (MAX_TREE_LEVELS - 1) >= 2^64.
 *
 * In practice, we'll need far fewer levels, because you will run out of
 * memory long before reaching that number, but let's be conservative.
 */
#define MAX_TREE_LEVELS		11

/*
 * Node structures, for the in-memory B-tree.
 *
 * An internal node holds a number of downlink pointers to leaf nodes, or
 * to internal nodes on a lower level.  For each downlink, the key value
 * corresponding to the lower level node is stored in a sorted array.  The
 * stored key values are low keys.  In other words, if the downlink has value
 * X, then all items stored on that child are >= X.
 *
 * Each leaf node holds a number of "items", with a varying number of
 * integers packed into each item.  Each item consists of two 64-bit words:
 * The first word holds the first integer stored in the item, in plain format.
 * The second word contains between 0 and 240 more integers, packed using
 * Simple-8b encoding.  By storing the first integer in plain, unpacked,
 * format, we can use binary search to quickly find an item that holds (or
 * would hold) a particular integer.  And by storing the rest in packed form,
 * we still get pretty good memory density, if there are clusters of integers
 * with similar values.
 *
 * Each leaf node also has a pointer to the next leaf node, so that the leaf
 * nodes can be easily walked from beginning to end when iterating.
 */
typedef struct intset_node intset_node;
typedef struct intset_leaf_node intset_leaf_node;
typedef struct intset_internal_node intset_internal_node;

/* Common structure of both leaf and internal nodes. */
struct intset_node
{
	uint16		level;			/* tree level of this node */
	uint16		num_items;		/* number of items in this node */
};

/* Internal node */
struct intset_internal_node
{
	/* common header, must match intset_node */
	uint16		level;			/* >= 1 on internal nodes */
	uint16		num_items;

	/*
	 * 'values' is an array of key values, and 'downlinks' are pointers to
	 * lower-level nodes, corresponding to the key values.
	 */
	uint64		values[MAX_INTERNAL_ITEMS];
	intset_node *downlinks[MAX_INTERNAL_ITEMS];
};

/* Leaf node */
typedef struct
{
	uint64		first;			/* first integer in this item */
	uint64		codeword;		/* simple8b encoded differences from 'first' */
} leaf_item;

#define MAX_VALUES_PER_LEAF_ITEM	(1 + SIMPLE8B_MAX_VALUES_PER_CODEWORD)

struct intset_leaf_node
{
	/* common header, must match intset_node */
	uint16		level;			/* 0 on leafs */
	uint16		num_items;

	intset_leaf_node *next;		/* right sibling, if any */

	leaf_item	items[MAX_LEAF_ITEMS];
};

/*
 * We buffer insertions in a simple array, before packing and inserting them
 * into the B-tree.  MAX_BUFFERED_VALUES sets the size of the buffer.  The
 * encoder assumes that it is large enough that we can always fill a leaf
 * item with buffered new items.  In other words, MAX_BUFFERED_VALUES must be
 * larger than MAX_VALUES_PER_LEAF_ITEM.  For efficiency, make it much larger.
 */
#define MAX_BUFFERED_VALUES			(MAX_VALUES_PER_LEAF_ITEM * 2)

/*
 * IntegerSet is the top-level object representing the set.
 *
 * The integers are stored in an in-memory B-tree structure, plus an array
 * for newly-added integers.  IntegerSet also tracks information about memory
 * usage, as well as the current position when iterating the set with
 * intset_begin_iterate / intset_iterate_next.
 */
struct IntegerSet
{
	/*
	 * 'context' is the memory context holding this integer set and all its
	 * tree nodes.
	 *
	 * 'mem_used' tracks the amount of memory used.  We don't do anything with
	 * it in integerset.c itself, but the callers can ask for it with
	 * intset_memory_usage().
	 */
	MemoryContext context;
	uint64		mem_used;

	uint64		num_entries;	/* total # of values in the set */
	uint64		highest_value;	/* highest value stored in this set */

	/*
	 * B-tree to hold the packed values.
	 *
	 * 'rightmost_nodes' hold pointers to the rightmost node on each level.
	 * rightmost_parent[0] is rightmost leaf, rightmost_parent[1] is its
	 * parent, and so forth, all the way up to the root. These are needed when
	 * adding new values. (Currently, we require that new values are added at
	 * the end.)
	 */
	int			num_levels;		/* height of the tree */
	intset_node *root;			/* root node */
	intset_node *rightmost_nodes[MAX_TREE_LEVELS];
	intset_leaf_node *leftmost_leaf;	/* leftmost leaf node */

	/*
	 * Holding area for new items that haven't been inserted to the tree yet.
	 */
	uint64		buffered_values[MAX_BUFFERED_VALUES];
	int			num_buffered_values;

	/*
	 * Iterator support.
	 *
	 * 'iter_values' is an array of integers ready to be returned to the
	 * caller; 'iter_num_values' is the length of that array, and
	 * 'iter_valueno' is the next index.  'iter_node' and 'iter_itemno' point
	 * to the leaf node, and item within the leaf node, to get the next batch
	 * of values from.
	 *
	 * Normally, 'iter_values' points to 'iter_values_buf', which holds items
	 * decoded from a leaf item.  But after we have scanned the whole B-tree,
	 * we iterate through all the unbuffered values, too, by pointing
	 * iter_values to 'buffered_values'.
	 */
	bool		iter_active;	/* is iteration in progress? */

	const uint64 *iter_values;
	int			iter_num_values;	/* number of elements in 'iter_values' */
	int			iter_valueno;	/* next index into 'iter_values' */

	intset_leaf_node *iter_node;	/* current leaf node */
	int			iter_itemno;	/* next item in 'iter_node' to decode */

	uint64		iter_values_buf[MAX_VALUES_PER_LEAF_ITEM];
};

/*
 * Prototypes for internal functions.
 */
static void intset_update_upper(IntegerSet *intset, int level,
								intset_node *child, uint64 child_key);
static void intset_flush_buffered_values(IntegerSet *intset);

static int	intset_binsrch_uint64(uint64 value, uint64 *arr, int arr_elems,
								  bool nextkey);
static int	intset_binsrch_leaf(uint64 value, leaf_item *arr, int arr_elems,
								bool nextkey);

static uint64 simple8b_encode(const uint64 *ints, int *num_encoded, uint64 base);
static int	simple8b_decode(uint64 codeword, uint64 *decoded, uint64 base);
static bool simple8b_contains(uint64 codeword, uint64 key, uint64 base);


/*
 * Create a new, initially empty, integer set.
 *
 * The integer set is created in the current memory context.
 * We will do all subsequent allocations in the same context, too, regardless
 * of which memory context is current when new integers are added to the set.
 */
IntegerSet *
intset_create(void)
{
	IntegerSet *intset;

	intset = (IntegerSet *) palloc(sizeof(IntegerSet));
	intset->context = CurrentMemoryContext;
	intset->mem_used = GetMemoryChunkSpace(intset);

	intset->num_entries = 0;
	intset->highest_value = 0;

	intset->num_levels = 0;
	intset->root = NULL;
	memset(intset->rightmost_nodes, 0, sizeof(intset->rightmost_nodes));
	intset->leftmost_leaf = NULL;

	intset->num_buffered_values = 0;

	intset->iter_active = false;
	intset->iter_node = NULL;
	intset->iter_itemno = 0;
	intset->iter_valueno = 0;
	intset->iter_num_values = 0;
	intset->iter_values = NULL;

	return intset;
}

/*
 * Allocate a new node.
 */
static intset_internal_node *
intset_new_internal_node(IntegerSet *intset)
{
	intset_internal_node *n;

	n = (intset_internal_node *) MemoryContextAlloc(intset->context,
													sizeof(intset_internal_node));
	intset->mem_used += GetMemoryChunkSpace(n);

	n->level = 0;				/* caller must set */
	n->num_items = 0;

	return n;
}

static intset_leaf_node *
intset_new_leaf_node(IntegerSet *intset)
{
	intset_leaf_node *n;

	n = (intset_leaf_node *) MemoryContextAlloc(intset->context,
												sizeof(intset_leaf_node));
	intset->mem_used += GetMemoryChunkSpace(n);

	n->level = 0;
	n->num_items = 0;
	n->next = NULL;

	return n;
}

/*
 * Return the number of entries in the integer set.
 */
uint64
intset_num_entries(IntegerSet *intset)
{
	return intset->num_entries;
}

/*
 * Return the amount of memory used by the integer set.
 */
uint64
intset_memory_usage(IntegerSet *intset)
{
	return intset->mem_used;
}

/*
 * Add a value to the set.
 *
 * Values must be added in order.
 */
void
intset_add_member(IntegerSet *intset, uint64 x)
{
	if (intset->iter_active)
		elog(ERROR, "cannot add new values to integer set while iteration is in progress");

	if (x <= intset->highest_value && intset->num_entries > 0)
		elog(ERROR, "cannot add value to integer set out of order");

	if (intset->num_buffered_values >= MAX_BUFFERED_VALUES)
	{
		/* Time to flush our buffer */
		intset_flush_buffered_values(intset);
		Assert(intset->num_buffered_values < MAX_BUFFERED_VALUES);
	}

	/* Add it to the buffer of newly-added values */
	intset->buffered_values[intset->num_buffered_values] = x;
	intset->num_buffered_values++;
	intset->num_entries++;
	intset->highest_value = x;
}

/*
 * Take a batch of buffered values, and pack them into the B-tree.
 */
static void
intset_flush_buffered_values(IntegerSet *intset)
{
	uint64	   *values = intset->buffered_values;
	uint64		num_values = intset->num_buffered_values;
	int			num_packed = 0;
	intset_leaf_node *leaf;

	leaf = (intset_leaf_node *) intset->rightmost_nodes[0];

	/*
	 * If the tree is completely empty, create the first leaf page, which is
	 * also the root.
	 */
	if (leaf == NULL)
	{
		/*
		 * This is the very first item in the set.
		 *
		 * Allocate root node. It's also a leaf.
		 */
		leaf = intset_new_leaf_node(intset);

		intset->root = (intset_node *) leaf;
		intset->leftmost_leaf = leaf;
		intset->rightmost_nodes[0] = (intset_node *) leaf;
		intset->num_levels = 1;
	}

	/*
	 * If there are less than MAX_VALUES_PER_LEAF_ITEM values in the buffer,
	 * stop.  In most cases, we cannot encode that many values in a single
	 * value, but this way, the encoder doesn't have to worry about running
	 * out of input.
	 */
	while (num_values - num_packed >= MAX_VALUES_PER_LEAF_ITEM)
	{
		leaf_item	item;
		int			num_encoded;

		/*
		 * Construct the next leaf item, packing as many buffered values as
		 * possible.
		 */
		item.first = values[num_packed];
		item.codeword = simple8b_encode(&values[num_packed + 1],
										&num_encoded,
										item.first);

		/*
		 * Add the item to the node, allocating a new node if the old one is
		 * full.
		 */
		if (leaf->num_items >= MAX_LEAF_ITEMS)
		{
			/* Allocate new leaf and link it to the tree */
			intset_leaf_node *old_leaf = leaf;

			leaf = intset_new_leaf_node(intset);
			old_leaf->next = leaf;
			intset->rightmost_nodes[0] = (intset_node *) leaf;
			intset_update_upper(intset, 1, (intset_node *) leaf, item.first);
		}
		leaf->items[leaf->num_items++] = item;

		num_packed += 1 + num_encoded;
	}

	/*
	 * Move any remaining buffered values to the beginning of the array.
	 */
	if (num_packed < intset->num_buffered_values)
	{
		memmove(&intset->buffered_values[0],
				&intset->buffered_values[num_packed],
				(intset->num_buffered_values - num_packed) * sizeof(uint64));
	}
	intset->num_buffered_values -= num_packed;
}

/*
 * Insert a downlink into parent node, after creating a new node.
 *
 * Recurses if the parent node is full, too.
 */
static void
intset_update_upper(IntegerSet *intset, int level, intset_node *child,
					uint64 child_key)
{
	intset_internal_node *parent;

	Assert(level > 0);

	/*
	 * Create a new root node, if necessary.
	 */
	if (level >= intset->num_levels)
	{
		intset_node *oldroot = intset->root;
		uint64		downlink_key;

		/* MAX_TREE_LEVELS should be more than enough, this shouldn't happen */
		if (intset->num_levels == MAX_TREE_LEVELS)
			elog(ERROR, "could not expand integer set, maximum number of levels reached");
		intset->num_levels++;

		/*
		 * Get the first value on the old root page, to be used as the
		 * downlink.
		 */
		if (intset->root->level == 0)
			downlink_key = ((intset_leaf_node *) oldroot)->items[0].first;
		else
			downlink_key = ((intset_internal_node *) oldroot)->values[0];

		parent = intset_new_internal_node(intset);
		parent->level = level;
		parent->values[0] = downlink_key;
		parent->downlinks[0] = oldroot;
		parent->num_items = 1;

		intset->root = (intset_node *) parent;
		intset->rightmost_nodes[level] = (intset_node *) parent;
	}

	/*
	 * Place the downlink on the parent page.
	 */
	parent = (intset_internal_node *) intset->rightmost_nodes[level];

	if (parent->num_items < MAX_INTERNAL_ITEMS)
	{
		parent->values[parent->num_items] = child_key;
		parent->downlinks[parent->num_items] = child;
		parent->num_items++;
	}
	else
	{
		/*
		 * Doesn't fit.  Allocate new parent, with the downlink as the first
		 * item on it, and recursively insert the downlink to the new parent
		 * to the grandparent.
		 */
		parent = intset_new_internal_node(intset);
		parent->level = level;
		parent->values[0] = child_key;
		parent->downlinks[0] = child;
		parent->num_items = 1;

		intset->rightmost_nodes[level] = (intset_node *) parent;

		intset_update_upper(intset, level + 1, (intset_node *) parent, child_key);
	}
}

/*
 * Does the set contain the given value?
 */
bool
intset_is_member(IntegerSet *intset, uint64 x)
{
	intset_node *node;
	intset_leaf_node *leaf;
	int			level;
	int			itemno;
	leaf_item  *item;

	/*
	 * The value might be in the buffer of newly-added values.
	 */
	if (intset->num_buffered_values > 0 && x >= intset->buffered_values[0])
	{
		int			itemno;

		itemno = intset_binsrch_uint64(x,
									   intset->buffered_values,
									   intset->num_buffered_values,
									   false);
		if (itemno >= intset->num_buffered_values)
			return false;
		else
			return (intset->buffered_values[itemno] == x);
	}

	/*
	 * Start from the root, and walk down the B-tree to find the right leaf
	 * node.
	 */
	if (!intset->root)
		return false;
	node = intset->root;
	for (level = intset->num_levels - 1; level > 0; level--)
	{
		intset_internal_node *n = (intset_internal_node *) node;

		Assert(node->level == level);

		itemno = intset_binsrch_uint64(x, n->values, n->num_items, true);
		if (itemno == 0)
			return false;
		node = n->downlinks[itemno - 1];
	}
	Assert(node->level == 0);
	leaf = (intset_leaf_node *) node;

	/*
	 * Binary search to find the right item on the leaf page
	 */
	itemno = intset_binsrch_leaf(x, leaf->items, leaf->num_items, true);
	if (itemno == 0)
		return false;
	item = &leaf->items[itemno - 1];

	/* Is this a match to the first value on the item? */
	if (item->first == x)
		return true;
	Assert(x > item->first);

	/* Is it in the packed codeword? */
	if (simple8b_contains(item->codeword, x, item->first))
		return true;

	return false;
}

/*
 * Begin in-order scan through all the values.
 *
 * While the iteration is in-progress, you cannot add new values to the set.
 */
void
intset_begin_iterate(IntegerSet *intset)
{
	/* Note that we allow an iteration to be abandoned midway */
	intset->iter_active = true;
	intset->iter_node = intset->leftmost_leaf;
	intset->iter_itemno = 0;
	intset->iter_valueno = 0;
	intset->iter_num_values = 0;
	intset->iter_values = intset->iter_values_buf;
}

/*
 * Returns the next integer, when iterating.
 *
 * intset_begin_iterate() must be called first.  intset_iterate_next() returns
 * the next value in the set.  Returns true, if there was another value, and
 * stores the value in *next.  Otherwise, returns false.
 */
bool
intset_iterate_next(IntegerSet *intset, uint64 *next)
{
	Assert(intset->iter_active);
	for (;;)
	{
		/* Return next iter_values[] entry if any */
		if (intset->iter_valueno < intset->iter_num_values)
		{
			*next = intset->iter_values[intset->iter_valueno++];
			return true;
		}

		/* Decode next item in current leaf node, if any */
		if (intset->iter_node &&
			intset->iter_itemno < intset->iter_node->num_items)
		{
			leaf_item  *item;
			int			num_decoded;

			item = &intset->iter_node->items[intset->iter_itemno++];

			intset->iter_values_buf[0] = item->first;
			num_decoded = simple8b_decode(item->codeword,
										  &intset->iter_values_buf[1],
										  item->first);
			intset->iter_num_values = num_decoded + 1;
			intset->iter_valueno = 0;
			continue;
		}

		/* No more items on this leaf, step to next node */
		if (intset->iter_node)
		{
			intset->iter_node = intset->iter_node->next;
			intset->iter_itemno = 0;
			continue;
		}

		/*
		 * We have reached the end of the B-tree.  But we might still have
		 * some integers in the buffer of newly-added values.
		 */
		if (intset->iter_values == (const uint64 *) intset->iter_values_buf)
		{
			intset->iter_values = intset->buffered_values;
			intset->iter_num_values = intset->num_buffered_values;
			intset->iter_valueno = 0;
			continue;
		}

		break;
	}

	/* No more results. */
	intset->iter_active = false;
	*next = 0;					/* prevent uninitialized-variable warnings */
	return false;
}

/*
 * intset_binsrch_uint64() -- search a sorted array of uint64s
 *
 * Returns the first position with key equal or less than the given key.
 * The returned position would be the "insert" location for the given key,
 * that is, the position where the new key should be inserted to.
 *
 * 'nextkey' affects the behavior on equal keys.  If true, and there is an
 * equal key in the array, this returns the position immediately after the
 * equal key.  If false, this returns the position of the equal key itself.
 */
static int
intset_binsrch_uint64(uint64 item, uint64 *arr, int arr_elems, bool nextkey)
{
	int			low,
				high,
				mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (nextkey)
		{
			if (item >= arr[mid])
				low = mid + 1;
			else
				high = mid;
		}
		else
		{
			if (item > arr[mid])
				low = mid + 1;
			else
				high = mid;
		}
	}

	return low;
}

/* same, but for an array of leaf items */
static int
intset_binsrch_leaf(uint64 item, leaf_item *arr, int arr_elems, bool nextkey)
{
	int			low,
				high,
				mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (nextkey)
		{
			if (item >= arr[mid].first)
				low = mid + 1;
			else
				high = mid;
		}
		else
		{
			if (item > arr[mid].first)
				low = mid + 1;
			else
				high = mid;
		}
	}

	return low;
}

/*
 * Simple-8b encoding.
 *
 * The simple-8b algorithm packs between 1 and 240 integers into 64-bit words,
 * called "codewords".  The number of integers packed into a single codeword
 * depends on the integers being packed; small integers are encoded using
 * fewer bits than large integers.  A single codeword can store a single
 * 60-bit integer, or two 30-bit integers, for example.
 *
 * Since we're storing a unique, sorted, set of integers, we actually encode
 * the *differences* between consecutive integers.  That way, clusters of
 * integers that are close to each other are packed efficiently, regardless
 * of their absolute values.
 *
 * In Simple-8b, each codeword consists of a 4-bit selector, which indicates
 * how many integers are encoded in the codeword, and the encoded integers are
 * packed into the remaining 60 bits.  The selector allows for 16 different
 * ways of using the remaining 60 bits, called "modes".  The number of integers
 * packed into a single codeword in each mode is listed in the simple8b_modes
 * table below.  For example, consider the following codeword:
 *
 *      20-bit integer       20-bit integer       20-bit integer
 * 1101 00000000000000010010 01111010000100100000 00000000000000010100
 * ^
 * selector
 *
 * The selector 1101 is 13 in decimal.  From the modes table below, we see
 * that it means that the codeword encodes three 20-bit integers.  In decimal,
 * those integers are 18, 500000 and 20.  Because we encode deltas rather than
 * absolute values, the actual values that they represent are 18, 500018 and
 * 500038.
 *
 * Modes 0 and 1 are a bit special; they encode a run of 240 or 120 zeroes
 * (which means 240 or 120 consecutive integers, since we're encoding the
 * deltas between integers), without using the rest of the codeword bits
 * for anything.
 *
 * Simple-8b cannot encode integers larger than 60 bits.  Values larger than
 * that are always stored in the 'first' field of a leaf item, never in the
 * packed codeword.  If there is a sequence of integers that are more than
 * 2^60 apart, the codeword will go unused on those items.  To represent that,
 * we use a magic EMPTY_CODEWORD codeword value.
 */
static const struct simple8b_mode
{
	uint8		bits_per_int;
	uint8		num_ints;
}			simple8b_modes[17] =

{
	{0, 240},					/* mode  0: 240 zeroes */
	{0, 120},					/* mode  1: 120 zeroes */
	{1, 60},					/* mode  2: sixty 1-bit integers */
	{2, 30},					/* mode  3: thirty 2-bit integers */
	{3, 20},					/* mode  4: twenty 3-bit integers */
	{4, 15},					/* mode  5: fifteen 4-bit integers */
	{5, 12},					/* mode  6: twelve 5-bit integers */
	{6, 10},					/* mode  7: ten 6-bit integers */
	{7, 8},						/* mode  8: eight 7-bit integers (four bits
								 * are wasted) */
	{8, 7},						/* mode  9: seven 8-bit integers (four bits
								 * are wasted) */
	{10, 6},					/* mode 10: six 10-bit integers */
	{12, 5},					/* mode 11: five 12-bit integers */
	{15, 4},					/* mode 12: four 15-bit integers */
	{20, 3},					/* mode 13: three 20-bit integers */
	{30, 2},					/* mode 14: two 30-bit integers */
	{60, 1},					/* mode 15: one 60-bit integer */

	{0, 0}						/* sentinel value */
};

/*
 * EMPTY_CODEWORD is a special value, used to indicate "no values".
 * It is used if the next value is too large to be encoded with Simple-8b.
 *
 * This value looks like a mode-0 codeword, but we can distinguish it
 * because a regular mode-0 codeword would have zeroes in the unused bits.
 */
#define EMPTY_CODEWORD		UINT64CONST(0x0FFFFFFFFFFFFFFF)

/*
 * Encode a number of integers into a Simple-8b codeword.
 *
 * (What we actually encode are deltas between successive integers.
 * "base" is the value before ints[0].)
 *
 * The input array must contain at least SIMPLE8B_MAX_VALUES_PER_CODEWORD
 * elements, ensuring that we can produce a full codeword.
 *
 * Returns the encoded codeword, and sets *num_encoded to the number of
 * input integers that were encoded.  That can be zero, if the first delta
 * is too large to be encoded.
 */
static uint64
simple8b_encode(const uint64 *ints, int *num_encoded, uint64 base)
{
	int			selector;
	int			nints;
	int			bits;
	uint64		diff;
	uint64		last_val;
	uint64		codeword;
	int			i;

	Assert(ints[0] > base);

	/*
	 * Select the "mode" to use for this codeword.
	 *
	 * In each iteration, check if the next value can be represented in the
	 * current mode we're considering.  If it's too large, then step up the
	 * mode to a wider one, and repeat.  If it fits, move on to the next
	 * integer.  Repeat until the codeword is full, given the current mode.
	 *
	 * Note that we don't have any way to represent unused slots in the
	 * codeword, so we require each codeword to be "full".  It is always
	 * possible to produce a full codeword unless the very first delta is too
	 * large to be encoded.  For example, if the first delta is small but the
	 * second is too large to be encoded, we'll end up using the last "mode",
	 * which has nints == 1.
	 */
	selector = 0;
	nints = simple8b_modes[0].num_ints;
	bits = simple8b_modes[0].bits_per_int;
	diff = ints[0] - base - 1;
	last_val = ints[0];
	i = 0;						/* number of deltas we have accepted */
	for (;;)
	{
		if (diff >= (UINT64CONST(1) << bits))
		{
			/* too large, step up to next mode */
			selector++;
			nints = simple8b_modes[selector].num_ints;
			bits = simple8b_modes[selector].bits_per_int;
			/* we might already have accepted enough deltas for this mode */
			if (i >= nints)
				break;
		}
		else
		{
			/* accept this delta; then done if codeword is full */
			i++;
			if (i >= nints)
				break;
			/* examine next delta */
			Assert(ints[i] > last_val);
			diff = ints[i] - last_val - 1;
			last_val = ints[i];
		}
	}

	if (nints == 0)
	{
		/*
		 * The first delta is too large to be encoded with Simple-8b.
		 *
		 * If there is at least one not-too-large integer in the input, we
		 * will encode it using mode 15 (or a more compact mode).  Hence, we
		 * can only get here if the *first* delta is >= 2^60.
		 */
		Assert(i == 0);
		*num_encoded = 0;
		return EMPTY_CODEWORD;
	}

	/*
	 * Encode the integers using the selected mode.  Note that we shift them
	 * into the codeword in reverse order, so that they will come out in the
	 * correct order in the decoder.
	 */
	codeword = 0;
	if (bits > 0)
	{
		for (i = nints - 1; i > 0; i--)
		{
			diff = ints[i] - ints[i - 1] - 1;
			codeword |= diff;
			codeword <<= bits;
		}
		diff = ints[0] - base - 1;
		codeword |= diff;
	}

	/* add selector to the codeword, and return */
	codeword |= (uint64) selector << 60;

	*num_encoded = nints;
	return codeword;
}

/*
 * Decode a codeword into an array of integers.
 * Returns the number of integers decoded.
 */
static int
simple8b_decode(uint64 codeword, uint64 *decoded, uint64 base)
{
	int			selector = (codeword >> 60);
	int			nints = simple8b_modes[selector].num_ints;
	int			bits = simple8b_modes[selector].bits_per_int;
	uint64		mask = (UINT64CONST(1) << bits) - 1;
	uint64		curr_value;

	if (codeword == EMPTY_CODEWORD)
		return 0;

	curr_value = base;
	for (int i = 0; i < nints; i++)
	{
		uint64		diff = codeword & mask;

		curr_value += 1 + diff;
		decoded[i] = curr_value;
		codeword >>= bits;
	}
	return nints;
}

/*
 * This is very similar to simple8b_decode(), but instead of decoding all
 * the values to an array, it just checks if the given "key" is part of
 * the codeword.
 */
static bool
simple8b_contains(uint64 codeword, uint64 key, uint64 base)
{
	int			selector = (codeword >> 60);
	int			nints = simple8b_modes[selector].num_ints;
	int			bits = simple8b_modes[selector].bits_per_int;

	if (codeword == EMPTY_CODEWORD)
		return false;

	if (bits == 0)
	{
		/* Special handling for 0-bit cases. */
		return (key - base) <= nints;
	}
	else
	{
		uint64		mask = (UINT64CONST(1) << bits) - 1;
		uint64		curr_value;

		curr_value = base;
		for (int i = 0; i < nints; i++)
		{
			uint64		diff = codeword & mask;

			curr_value += 1 + diff;

			if (curr_value >= key)
			{
				if (curr_value == key)
					return true;
				else
					return false;
			}

			codeword >>= bits;
		}
	}
	return false;
}
