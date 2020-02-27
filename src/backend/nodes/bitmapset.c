/*-------------------------------------------------------------------------
 *
 * bitmapset.c
 *	  PostgreSQL generic bitmap set package
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, a NULL pointer is always
 * accepted by all operations to represent the empty set.  (But beware
 * that this is not the only representation of the empty set.  Use
 * bms_is_empty() in preference to testing for NULL.)
 *
 *
 * Copyright (c) 2003-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/nodes/bitmapset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "port/pg_bitutils.h"


#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

#define BITMAPSET_SIZE(nwords)	\
	(offsetof(Bitmapset, words) + (nwords) * sizeof(bitmapword))

/*----------
 * This is a well-known cute trick for isolating the rightmost one-bit
 * in a word.  It assumes two's complement arithmetic.  Consider any
 * nonzero value, and focus attention on the rightmost one.  The value is
 * then something like
 *				xxxxxx10000
 * where x's are unspecified bits.  The two's complement negative is formed
 * by inverting all the bits and adding one.  Inversion gives
 *				yyyyyy01111
 * where each y is the inverse of the corresponding x.  Incrementing gives
 *				yyyyyy10000
 * and then ANDing with the original value gives
 *				00000010000
 * This works for all cases except original value = zero, where of course
 * we get zero.
 *----------
 */
#define RIGHTMOST_ONE(x) ((signedbitmapword) (x) & -((signedbitmapword) (x)))

#define HAS_MULTIPLE_ONES(x)	((bitmapword) RIGHTMOST_ONE(x) != (x))

/* Select appropriate bit-twiddling functions for bitmap word size */
#if BITS_PER_BITMAPWORD == 32
#define bmw_leftmost_one_pos(w)		pg_leftmost_one_pos32(w)
#define bmw_rightmost_one_pos(w)	pg_rightmost_one_pos32(w)
#define bmw_popcount(w)				pg_popcount32(w)
#elif BITS_PER_BITMAPWORD == 64
#define bmw_leftmost_one_pos(w)		pg_leftmost_one_pos64(w)
#define bmw_rightmost_one_pos(w)	pg_rightmost_one_pos64(w)
#define bmw_popcount(w)				pg_popcount64(w)
#else
#error "invalid BITS_PER_BITMAPWORD"
#endif


/*
 * bms_copy - make a palloc'd copy of a bitmapset
 */
Bitmapset *
bms_copy(const Bitmapset *a)
{
	Bitmapset  *result;
	size_t		size;

	if (a == NULL)
		return NULL;
	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) palloc(size);
	memcpy(result, a, size);
	return result;
}

/*
 * bms_equal - are two bitmapsets equal?
 *
 * This is logical not physical equality; in particular, a NULL pointer will
 * be reported as equal to a palloc'd value containing no members.
 */
bool
bms_equal(const Bitmapset *a, const Bitmapset *b)
{
	const Bitmapset *shorter;
	const Bitmapset *longer;
	int			shortlen;
	int			longlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return true;
		return bms_is_empty(b);
	}
	else if (b == NULL)
		return bms_is_empty(a);
	/* Identify shorter and longer input */
	if (a->nwords <= b->nwords)
	{
		shorter = a;
		longer = b;
	}
	else
	{
		shorter = b;
		longer = a;
	}
	/* And process */
	shortlen = shorter->nwords;
	for (i = 0; i < shortlen; i++)
	{
		if (shorter->words[i] != longer->words[i])
			return false;
	}
	longlen = longer->nwords;
	for (; i < longlen; i++)
	{
		if (longer->words[i] != 0)
			return false;
	}
	return true;
}

/*
 * bms_compare - qsort-style comparator for bitmapsets
 *
 * This guarantees to report values as equal iff bms_equal would say they are
 * equal.  Otherwise, the highest-numbered bit that is set in one value but
 * not the other determines the result.  (This rule means that, for example,
 * {6} is greater than {5}, which seems plausible.)
 */
int
bms_compare(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_is_empty(b) ? 0 : -1;
	else if (b == NULL)
		return bms_is_empty(a) ? 0 : +1;
	/* Handle cases where one input is longer than the other */
	shortlen = Min(a->nwords, b->nwords);
	for (i = shortlen; i < a->nwords; i++)
	{
		if (a->words[i] != 0)
			return +1;
	}
	for (i = shortlen; i < b->nwords; i++)
	{
		if (b->words[i] != 0)
			return -1;
	}
	/* Process words in common */
	i = shortlen;
	while (--i >= 0)
	{
		bitmapword	aw = a->words[i];
		bitmapword	bw = b->words[i];

		if (aw != bw)
			return (aw > bw) ? +1 : -1;
	}
	return 0;
}

/*
 * bms_make_singleton - build a bitmapset containing a single member
 */
Bitmapset *
bms_make_singleton(int x)
{
	Bitmapset  *result;
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	result = (Bitmapset *) palloc0(BITMAPSET_SIZE(wordnum + 1));
	result->nwords = wordnum + 1;
	result->words[wordnum] = ((bitmapword) 1 << bitnum);
	return result;
}

/*
 * bms_free - free a bitmapset
 *
 * Same as pfree except for allowing NULL input
 */
void
bms_free(Bitmapset *a)
{
	if (a)
		pfree(a);
}


/*
 * These operations all make a freshly palloc'd result,
 * leaving their inputs untouched
 */


/*
 * bms_union - set union
 */
Bitmapset *
bms_union(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return bms_copy(a);
	/* Identify shorter and longer input; copy the longer one */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = bms_copy(a);
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	return result;
}

/*
 * bms_intersect - set intersection
 */
Bitmapset *
bms_intersect(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			resultlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL || b == NULL)
		return NULL;
	/* Identify shorter and longer input; copy the shorter one */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(a);
		other = b;
	}
	else
	{
		result = bms_copy(b);
		other = a;
	}
	/* And intersect the longer input with the result */
	resultlen = result->nwords;
	for (i = 0; i < resultlen; i++)
		result->words[i] &= other->words[i];
	return result;
}

/*
 * bms_difference - set difference (ie, A without members of B)
 */
Bitmapset *
bms_difference(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return bms_copy(a);
	/* Copy the left input */
	result = bms_copy(a);
	/* And remove b's bits from result */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		result->words[i] &= ~b->words[i];
	return result;
}

/*
 * bms_is_subset - is A a subset of B?
 */
bool
bms_is_subset(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			longlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return true;			/* empty set is a subset of anything */
	if (b == NULL)
		return bms_is_empty(a);
	/* Check common words */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return false;
	}
	/* Check extra words */
	if (a->nwords > b->nwords)
	{
		longlen = a->nwords;
		for (; i < longlen; i++)
		{
			if (a->words[i] != 0)
				return false;
		}
	}
	return true;
}

/*
 * bms_subset_compare - compare A and B for equality/subset relationships
 *
 * This is more efficient than testing bms_is_subset in both directions.
 */
BMS_Comparison
bms_subset_compare(const Bitmapset *a, const Bitmapset *b)
{
	BMS_Comparison result;
	int			shortlen;
	int			longlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return BMS_EQUAL;
		return bms_is_empty(b) ? BMS_EQUAL : BMS_SUBSET1;
	}
	if (b == NULL)
		return bms_is_empty(a) ? BMS_EQUAL : BMS_SUBSET2;
	/* Check common words */
	result = BMS_EQUAL;			/* status so far */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		bitmapword	aword = a->words[i];
		bitmapword	bword = b->words[i];

		if ((aword & ~bword) != 0)
		{
			/* a is not a subset of b */
			if (result == BMS_SUBSET1)
				return BMS_DIFFERENT;
			result = BMS_SUBSET2;
		}
		if ((bword & ~aword) != 0)
		{
			/* b is not a subset of a */
			if (result == BMS_SUBSET2)
				return BMS_DIFFERENT;
			result = BMS_SUBSET1;
		}
	}
	/* Check extra words */
	if (a->nwords > b->nwords)
	{
		longlen = a->nwords;
		for (; i < longlen; i++)
		{
			if (a->words[i] != 0)
			{
				/* a is not a subset of b */
				if (result == BMS_SUBSET1)
					return BMS_DIFFERENT;
				result = BMS_SUBSET2;
			}
		}
	}
	else if (a->nwords < b->nwords)
	{
		longlen = b->nwords;
		for (; i < longlen; i++)
		{
			if (b->words[i] != 0)
			{
				/* b is not a subset of a */
				if (result == BMS_SUBSET2)
					return BMS_DIFFERENT;
				result = BMS_SUBSET1;
			}
		}
	}
	return result;
}

/*
 * bms_is_member - is X a member of A?
 */
bool
bms_is_member(int x, const Bitmapset *a)
{
	int			wordnum,
				bitnum;

	/* XXX better to just return false for x<0 ? */
	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return false;
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum >= a->nwords)
		return false;
	if ((a->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
		return true;
	return false;
}

/*
 * bms_member_index
 *		determine 0-based index of member x in the bitmap
 *
 * Returns (-1) when x is not a member.
 */
int
bms_member_index(Bitmapset *a, int x)
{
	int			i;
	int			bitnum;
	int			wordnum;
	int			result = 0;
	bitmapword	mask;

	/* return -1 if not a member of the bitmap */
	if (!bms_is_member(x, a))
		return -1;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* count bits in preceding words */
	for (i = 0; i < wordnum; i++)
	{
		bitmapword	w = a->words[i];

		/* No need to count the bits in a zero word */
		if (w != 0)
			result += bmw_popcount(w);
	}

	/*
	 * Now add bits of the last word, but only those before the item. We can
	 * do that by applying a mask and then using popcount again. To get
	 * 0-based index, we want to count only preceding bits, not the item
	 * itself, so we subtract 1.
	 */
	mask = ((bitmapword) 1 << bitnum) - 1;
	result += bmw_popcount(a->words[wordnum] & mask);

	return result;
}

/*
 * bms_overlap - do sets overlap (ie, have a nonempty intersection)?
 */
bool
bms_overlap(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL || b == NULL)
		return false;
	/* Check words in common */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & b->words[i]) != 0)
			return true;
	}
	return false;
}

/*
 * bms_overlap_list - does a set overlap an integer list?
 */
bool
bms_overlap_list(const Bitmapset *a, const List *b)
{
	ListCell   *lc;
	int			wordnum,
				bitnum;

	if (a == NULL || b == NIL)
		return false;

	foreach(lc, b)
	{
		int			x = lfirst_int(lc);

		if (x < 0)
			elog(ERROR, "negative bitmapset member not allowed");
		wordnum = WORDNUM(x);
		bitnum = BITNUM(x);
		if (wordnum < a->nwords)
			if ((a->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
				return true;
	}

	return false;
}

/*
 * bms_nonempty_difference - do sets have a nonempty difference?
 */
bool
bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return false;
	if (b == NULL)
		return !bms_is_empty(a);
	/* Check words in common */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return true;
	}
	/* Check extra words in a */
	for (; i < a->nwords; i++)
	{
		if (a->words[i] != 0)
			return true;
	}
	return false;
}

/*
 * bms_singleton_member - return the sole integer member of set
 *
 * Raises error if |a| is not 1.
 */
int
bms_singleton_member(const Bitmapset *a)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		elog(ERROR, "bitmapset is empty");
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				elog(ERROR, "bitmapset has multiple members");
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	}
	if (result < 0)
		elog(ERROR, "bitmapset is empty");
	return result;
}

/*
 * bms_get_singleton_member
 *
 * Test whether the given set is a singleton.
 * If so, set *member to the value of its sole member, and return true.
 * If not, return false, without changing *member.
 *
 * This is more convenient and faster than calling bms_membership() and then
 * bms_singleton_member(), if we don't care about distinguishing empty sets
 * from multiple-member sets.
 */
bool
bms_get_singleton_member(const Bitmapset *a, int *member)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return false;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				return false;
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	}
	if (result < 0)
		return false;
	*member = result;
	return true;
}

/*
 * bms_num_members - count members of set
 */
int
bms_num_members(const Bitmapset *a)
{
	int			result = 0;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return 0;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		/* No need to count the bits in a zero word */
		if (w != 0)
			result += bmw_popcount(w);
	}
	return result;
}

/*
 * bms_membership - does a set have zero, one, or multiple members?
 *
 * This is faster than making an exact count with bms_num_members().
 */
BMS_Membership
bms_membership(const Bitmapset *a)
{
	BMS_Membership result = BMS_EMPTY_SET;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return BMS_EMPTY_SET;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result != BMS_EMPTY_SET || HAS_MULTIPLE_ONES(w))
				return BMS_MULTIPLE;
			result = BMS_SINGLETON;
		}
	}
	return result;
}

/*
 * bms_is_empty - is a set empty?
 *
 * This is even faster than bms_membership().
 */
bool
bms_is_empty(const Bitmapset *a)
{
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return true;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
			return false;
	}
	return true;
}


/*
 * These operations all "recycle" their non-const inputs, ie, either
 * return the modified input or pfree it if it can't hold the result.
 *
 * These should generally be used in the style
 *
 *		foo = bms_add_member(foo, x);
 */


/*
 * bms_add_member - add a specified member to set
 *
 * Input set is modified or recycled!
 */
Bitmapset *
bms_add_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return bms_make_singleton(x);
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* enlarge the set if necessary */
	if (wordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(wordnum + 1));
		a->nwords = wordnum + 1;
		/* zero out the enlarged portion */
		for (i = oldnwords; i < a->nwords; i++)
			a->words[i] = 0;
	}

	a->words[wordnum] |= ((bitmapword) 1 << bitnum);
	return a;
}

/*
 * bms_del_member - remove a specified member from set
 *
 * No error if x is not currently a member of set
 *
 * Input set is modified in-place!
 */
Bitmapset *
bms_del_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return NULL;
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum < a->nwords)
		a->words[wordnum] &= ~((bitmapword) 1 << bitnum);
	return a;
}

/*
 * bms_add_members - like bms_union, but left input is recycled
 */
Bitmapset *
bms_add_members(Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return a;
	/* Identify shorter and longer input; copy the longer one if needed */
	if (a->nwords < b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	if (result != a)
		pfree(a);
	return result;
}

/*
 * bms_add_range
 *		Add members in the range of 'lower' to 'upper' to the set.
 *
 * Note this could also be done by calling bms_add_member in a loop, however,
 * using this function will be faster when the range is large as we work at
 * the bitmapword level rather than at bit level.
 */
Bitmapset *
bms_add_range(Bitmapset *a, int lower, int upper)
{
	int			lwordnum,
				lbitnum,
				uwordnum,
				ushiftbits,
				wordnum;

	/* do nothing if nothing is called for, without further checking */
	if (upper < lower)
		return a;

	if (lower < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) palloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = uwordnum + 1;
	}
	else if (uwordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		/* ensure we have enough words to store the upper bit */
		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = uwordnum + 1;
		/* zero out the enlarged portion */
		for (i = oldnwords; i < a->nwords; i++)
			a->words[i] = 0;
	}

	wordnum = lwordnum = WORDNUM(lower);

	lbitnum = BITNUM(lower);
	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(upper) + 1);

	/*
	 * Special case when lwordnum is the same as uwordnum we must perform the
	 * upper and lower masking on the word.
	 */
	if (lwordnum == uwordnum)
	{
		a->words[lwordnum] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1)
			& (~(bitmapword) 0) >> ushiftbits;
	}
	else
	{
		/* turn on lbitnum and all bits left of it */
		a->words[wordnum++] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1);

		/* turn on all bits for any intermediate words */
		while (wordnum < uwordnum)
			a->words[wordnum++] = ~(bitmapword) 0;

		/* turn on upper's bit and all bits right of it. */
		a->words[uwordnum] |= (~(bitmapword) 0) >> ushiftbits;
	}

	return a;
}

/*
 * bms_int_members - like bms_intersect, but left input is recycled
 */
Bitmapset *
bms_int_members(Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
	{
		pfree(a);
		return NULL;
	}
	/* Intersect b into a; we need never copy */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		a->words[i] &= b->words[i];
	for (; i < a->nwords; i++)
		a->words[i] = 0;
	return a;
}

/*
 * bms_del_members - like bms_difference, but left input is recycled
 */
Bitmapset *
bms_del_members(Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return a;
	/* Remove b's bits from a; we need never copy */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		a->words[i] &= ~b->words[i];
	return a;
}

/*
 * bms_join - like bms_union, but *both* inputs are recycled
 */
Bitmapset *
bms_join(Bitmapset *a, Bitmapset *b)
{
	Bitmapset  *result;
	Bitmapset  *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;
	/* Identify shorter and longer input; use longer one as result */
	if (a->nwords < b->nwords)
	{
		result = b;
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	if (other != result)		/* pure paranoia */
		pfree(other);
	return result;
}

/*
 * bms_first_member - find and remove first member of a set
 *
 * Returns -1 if set is empty.  NB: set is destructively modified!
 *
 * This is intended as support for iterating through the members of a set.
 * The typical pattern is
 *
 *			while ((x = bms_first_member(inputset)) >= 0)
 *				process member x;
 *
 * CAUTION: this destroys the content of "inputset".  If the set must
 * not be modified, use bms_next_member instead.
 */
int
bms_first_member(Bitmapset *a)
{
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return -1;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			int			result;

			w = RIGHTMOST_ONE(w);
			a->words[wordnum] &= ~w;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
			return result;
		}
	}
	return -1;
}

/*
 * bms_next_member - find next member of a set
 *
 * Returns smallest member greater than "prevbit", or -2 if there is none.
 * "prevbit" must NOT be less than -1, or the behavior is unpredictable.
 *
 * This is intended as support for iterating through the members of a set.
 * The typical pattern is
 *
 *			x = -1;
 *			while ((x = bms_next_member(inputset, x)) >= 0)
 *				process member x;
 *
 * Notice that when there are no more members, we return -2, not -1 as you
 * might expect.  The rationale for that is to allow distinguishing the
 * loop-not-started state (x == -1) from the loop-completed state (x == -2).
 * It makes no difference in simple loop usage, but complex iteration logic
 * might need such an ability.
 */
int
bms_next_member(const Bitmapset *a, int prevbit)
{
	int			nwords;
	int			wordnum;
	bitmapword	mask;

	if (a == NULL)
		return -2;
	nwords = a->nwords;
	prevbit++;
	mask = (~(bitmapword) 0) << BITNUM(prevbit);
	for (wordnum = WORDNUM(prevbit); wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		/* ignore bits before prevbit */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
			return result;
		}

		/* in subsequent words, consider all bits */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_prev_member - find prev member of a set
 *
 * Returns largest member less than "prevbit", or -2 if there is none.
 * "prevbit" must NOT be more than one above the highest possible bit that can
 * be set at the Bitmapset at its current size.
 *
 * To ease finding the highest set bit for the initial loop, the special
 * prevbit value of -1 can be passed to have the function find the highest
 * valued member in the set.
 *
 * This is intended as support for iterating through the members of a set in
 * reverse.  The typical pattern is
 *
 *			x = -1;
 *			while ((x = bms_prev_member(inputset, x)) >= 0)
 *				process member x;
 *
 * Notice that when there are no more members, we return -2, not -1 as you
 * might expect.  The rationale for that is to allow distinguishing the
 * loop-not-started state (x == -1) from the loop-completed state (x == -2).
 * It makes no difference in simple loop usage, but complex iteration logic
 * might need such an ability.
 */

int
bms_prev_member(const Bitmapset *a, int prevbit)
{
	int			wordnum;
	int			ushiftbits;
	bitmapword	mask;

	/*
	 * If set is NULL or if there are no more bits to the right then we've
	 * nothing to do.
	 */
	if (a == NULL || prevbit == 0)
		return -2;

	/* transform -1 to the highest possible bit we could have set */
	if (prevbit == -1)
		prevbit = a->nwords * BITS_PER_BITMAPWORD - 1;
	else
		prevbit--;

	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(prevbit) + 1);
	mask = (~(bitmapword) 0) >> ushiftbits;
	for (wordnum = WORDNUM(prevbit); wordnum >= 0; wordnum--)
	{
		bitmapword	w = a->words[wordnum];

		/* mask out bits left of prevbit */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_leftmost_one_pos(w);
			return result;
		}

		/* in subsequent words, consider all bits */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_hash_value - compute a hash key for a Bitmapset
 *
 * Note: we must ensure that any two bitmapsets that are bms_equal() will
 * hash to the same value; in practice this means that trailing all-zero
 * words must not affect the result.  Hence we strip those before applying
 * hash_any().
 */
uint32
bms_hash_value(const Bitmapset *a)
{
	int			lastword;

	if (a == NULL)
		return 0;				/* All empty sets hash to 0 */
	for (lastword = a->nwords; --lastword >= 0;)
	{
		if (a->words[lastword] != 0)
			break;
	}
	if (lastword < 0)
		return 0;				/* All empty sets hash to 0 */
	return DatumGetUInt32(hash_any((const unsigned char *) a->words,
								   (lastword + 1) * sizeof(bitmapword)));
}

/*
 * bitmap_hash - hash function for keys that are (pointers to) Bitmapsets
 *
 * Note: don't forget to specify bitmap_match as the match function!
 */
uint32
bitmap_hash(const void *key, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return bms_hash_value(*((const Bitmapset *const *) key));
}

/*
 * bitmap_match - match function to use with bitmap_hash
 */
int
bitmap_match(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return !bms_equal(*((const Bitmapset *const *) key1),
					  *((const Bitmapset *const *) key2));
}
