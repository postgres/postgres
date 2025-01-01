/*-------------------------------------------------------------------------
 *
 * bitmapset.c
 *	  PostgreSQL generic bitmap set package
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, we always represent a set with
 * the minimum possible number of words, i.e, there are never any trailing
 * zero words.  Enforcing this requires that an empty set is represented as
 * NULL.  Because an empty Bitmapset is represented as NULL, a non-NULL
 * Bitmapset always has at least 1 Bitmapword.  We can exploit this fact to
 * speed up various loops over the Bitmapset's words array by using "do while"
 * loops instead of "for" loops.  This means the code does not waste time
 * checking the loop condition before the first iteration.  For Bitmapsets
 * containing only a single word (likely the majority of them) this halves the
 * number of loop condition checks.
 *
 * Callers must ensure that the set returned by functions in this file which
 * adjust the members of an existing set is assigned to all pointers pointing
 * to that existing set.  No guarantees are made that we'll ever modify the
 * existing set in-place and return it.
 *
 * To help find bugs caused by callers failing to record the return value of
 * the function which manipulates an existing set, we support building with
 * REALLOCATE_BITMAPSETS.  This results in the set being reallocated each time
 * the set is altered and the existing being pfreed.  This is useful as if any
 * references still exist to the old set, we're more likely to notice as
 * any users of the old set will be accessing pfree'd memory.  This option is
 * only intended to be used for debugging.
 *
 * Copyright (c) 2003-2025, PostgreSQL Global Development Group
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

#ifdef USE_ASSERT_CHECKING
/*
 * bms_is_valid_set - for cassert builds to check for valid sets
 */
static bool
bms_is_valid_set(const Bitmapset *a)
{
	/* NULL is the correct representation of an empty set */
	if (a == NULL)
		return true;

	/* check the node tag is set correctly.  pfree'd pointer, maybe? */
	if (!IsA(a, Bitmapset))
		return false;

	/* trailing zero words are not allowed */
	if (a->words[a->nwords - 1] == 0)
		return false;

	return true;
}
#endif

#ifdef REALLOCATE_BITMAPSETS
/*
 * bms_copy_and_free
 *		Only required in REALLOCATE_BITMAPSETS builds.  Provide a simple way
 *		to return a freshly allocated set and pfree the original.
 *
 * Note: callers which accept multiple sets must be careful when calling this
 * function to clone one parameter as other parameters may point to the same
 * set.  A good option is to call this just before returning the resulting
 * set.
 */
static Bitmapset *
bms_copy_and_free(Bitmapset *a)
{
	Bitmapset  *c = bms_copy(a);

	bms_free(a);
	return c;
}
#endif

/*
 * bms_copy - make a palloc'd copy of a bitmapset
 */
Bitmapset *
bms_copy(const Bitmapset *a)
{
	Bitmapset  *result;
	size_t		size;

	Assert(bms_is_valid_set(a));

	if (a == NULL)
		return NULL;

	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) palloc(size);
	memcpy(result, a, size);
	return result;
}

/*
 * bms_equal - are two bitmapsets equal? or both NULL?
 */
bool
bms_equal(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return true;
		return false;
	}
	else if (b == NULL)
		return false;

	/* can't be equal if the word counts don't match */
	if (a->nwords != b->nwords)
		return false;

	/* check each word matches */
	i = 0;
	do
	{
		if (a->words[i] != b->words[i])
			return false;
	} while (++i < a->nwords);

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
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return (b == NULL) ? 0 : -1;
	else if (b == NULL)
		return +1;

	/* the set with the most words must be greater */
	if (a->nwords != b->nwords)
		return (a->nwords > b->nwords) ? +1 : -1;

	i = a->nwords - 1;
	do
	{
		bitmapword	aw = a->words[i];
		bitmapword	bw = b->words[i];

		if (aw != bw)
			return (aw > bw) ? +1 : -1;
	} while (--i >= 0);
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
	result->type = T_Bitmapset;
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
 * bms_union - create and return a new set containing all members from both
 * input sets.  Both inputs are left unmodified.
 */
Bitmapset *
bms_union(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

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
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	return result;
}

/*
 * bms_intersect - create and return a new set containing members which both
 * input sets have in common.  Both inputs are left unmodified.
 */
Bitmapset *
bms_intersect(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			lastnonzero;
	int			resultlen;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

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
	lastnonzero = -1;
	i = 0;
	do
	{
		result->words[i] &= other->words[i];

		if (result->words[i] != 0)
			lastnonzero = i;
	} while (++i < resultlen);
	/* If we computed an empty result, we must return NULL */
	if (lastnonzero == -1)
	{
		pfree(result);
		return NULL;
	}

	/* get rid of trailing zero words */
	result->nwords = lastnonzero + 1;
	return result;
}

/*
 * bms_difference - create and return a new set containing all the members of
 * 'a' without the members of 'b'.
 */
Bitmapset *
bms_difference(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return bms_copy(a);

	/*
	 * In Postgres' usage, an empty result is a very common case, so it's
	 * worth optimizing for that by testing bms_nonempty_difference().  This
	 * saves us a palloc/pfree cycle compared to checking after-the-fact.
	 */
	if (!bms_nonempty_difference(a, b))
		return NULL;

	/* Copy the left input */
	result = bms_copy(a);

	/* And remove b's bits from result */
	if (result->nwords > b->nwords)
	{
		/*
		 * We'll never need to remove trailing zero words when 'a' has more
		 * words than 'b' as the additional words must be non-zero.
		 */
		i = 0;
		do
		{
			result->words[i] &= ~b->words[i];
		} while (++i < b->nwords);
	}
	else
	{
		int			lastnonzero = -1;

		/* we may need to remove trailing zero words from the result. */
		i = 0;
		do
		{
			result->words[i] &= ~b->words[i];

			/* remember the last non-zero word */
			if (result->words[i] != 0)
				lastnonzero = i;
		} while (++i < result->nwords);

		/* trim off trailing zero words */
		result->nwords = lastnonzero + 1;
	}
	Assert(result->nwords != 0);

	/* Need not check for empty result, since we handled that case above */
	return result;
}

/*
 * bms_is_subset - is A a subset of B?
 */
bool
bms_is_subset(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return true;			/* empty set is a subset of anything */
	if (b == NULL)
		return false;

	/* 'a' can't be a subset of 'b' if it contains more words */
	if (a->nwords > b->nwords)
		return false;

	/* Check all 'a' members are set in 'b' */
	i = 0;
	do
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return false;
	} while (++i < a->nwords);
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
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return BMS_EQUAL;
		return BMS_SUBSET1;
	}
	if (b == NULL)
		return BMS_SUBSET2;

	/* Check common words */
	result = BMS_EQUAL;			/* status so far */
	shortlen = Min(a->nwords, b->nwords);
	i = 0;
	do
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
	} while (++i < shortlen);
	/* Check extra words */
	if (a->nwords > b->nwords)
	{
		/* if a has more words then a is not a subset of b */
		if (result == BMS_SUBSET1)
			return BMS_DIFFERENT;
		return BMS_SUBSET2;
	}
	else if (a->nwords < b->nwords)
	{
		/* if b has more words then b is not a subset of a */
		if (result == BMS_SUBSET2)
			return BMS_DIFFERENT;
		return BMS_SUBSET1;
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

	Assert(bms_is_valid_set(a));

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

	Assert(bms_is_valid_set(a));

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

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL || b == NULL)
		return false;
	/* Check words in common */
	shortlen = Min(a->nwords, b->nwords);
	i = 0;
	do
	{
		if ((a->words[i] & b->words[i]) != 0)
			return true;
	} while (++i < shortlen);
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

	Assert(bms_is_valid_set(a));

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
 *
 * i.e., are any members set in 'a' that are not also set in 'b'.
 */
bool
bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return false;
	if (b == NULL)
		return true;
	/* if 'a' has more words then it must contain additional members */
	if (a->nwords > b->nwords)
		return true;
	/* Check all 'a' members are set in 'b' */
	i = 0;
	do
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return true;
	} while (++i < a->nwords);
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

	Assert(bms_is_valid_set(a));

	if (a == NULL)
		elog(ERROR, "bitmapset is empty");

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				elog(ERROR, "bitmapset has multiple members");
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	/* we don't expect non-NULL sets to be empty */
	Assert(result >= 0);
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

	Assert(bms_is_valid_set(a));

	if (a == NULL)
		return false;

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				return false;
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	/* we don't expect non-NULL sets to be empty */
	Assert(result >= 0);
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

	Assert(bms_is_valid_set(a));

	if (a == NULL)
		return 0;

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		/* No need to count the bits in a zero word */
		if (w != 0)
			result += bmw_popcount(w);
	} while (++wordnum < nwords);
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

	Assert(bms_is_valid_set(a));

	if (a == NULL)
		return BMS_EMPTY_SET;

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result != BMS_EMPTY_SET || HAS_MULTIPLE_ONES(w))
				return BMS_MULTIPLE;
			result = BMS_SINGLETON;
		}
	} while (++wordnum < nwords);
	return result;
}


/*
 * bms_add_member - add a specified member to set
 *
 * 'a' is recycled when possible.
 */
Bitmapset *
bms_add_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	Assert(bms_is_valid_set(a));

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
		i = oldnwords;
		do
		{
			a->words[i] = 0;
		} while (++i < a->nwords);
	}

	a->words[wordnum] |= ((bitmapword) 1 << bitnum);

#ifdef REALLOCATE_BITMAPSETS

	/*
	 * There's no guarantee that the repalloc returned a new pointer, so copy
	 * and free unconditionally here.
	 */
	a = bms_copy_and_free(a);
#endif

	return a;
}

/*
 * bms_del_member - remove a specified member from set
 *
 * No error if x is not currently a member of set
 *
 * 'a' is recycled when possible.
 */
Bitmapset *
bms_del_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	Assert(bms_is_valid_set(a));

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return NULL;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

#ifdef REALLOCATE_BITMAPSETS
	a = bms_copy_and_free(a);
#endif

	/* member can't exist.  Return 'a' unmodified */
	if (unlikely(wordnum >= a->nwords))
		return a;

	a->words[wordnum] &= ~((bitmapword) 1 << bitnum);

	/* when last word becomes empty, trim off all trailing empty words */
	if (a->words[wordnum] == 0 && wordnum == a->nwords - 1)
	{
		/* find the last non-empty word and make that the new final word */
		for (int i = wordnum - 1; i >= 0; i--)
		{
			if (a->words[i] != 0)
			{
				a->nwords = i + 1;
				return a;
			}
		}

		/* the set is now empty */
		pfree(a);
		return NULL;
	}
	return a;
}

/*
 * bms_add_members - like bms_union, but left input is recycled when possible
 */
Bitmapset *
bms_add_members(Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
	{
#ifdef REALLOCATE_BITMAPSETS
		a = bms_copy_and_free(a);
#endif

		return a;
	}
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
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	if (result != a)
		pfree(a);
#ifdef REALLOCATE_BITMAPSETS
	else
		result = bms_copy_and_free(result);
#endif

	return result;
}

/*
 * bms_replace_members
 *		Remove all existing members from 'a' and repopulate the set with members
 *		from 'b', recycling 'a', when possible.
 */
Bitmapset *
bms_replace_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
	{
		pfree(a);
		return NULL;
	}

	if (a->nwords < b->nwords)
		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(b->nwords));

	i = 0;
	do
	{
		a->words[i] = b->words[i];
	} while (++i < b->nwords);

	a->nwords = b->nwords;

#ifdef REALLOCATE_BITMAPSETS

	/*
	 * There's no guarantee that the repalloc returned a new pointer, so copy
	 * and free unconditionally here.
	 */
	a = bms_copy_and_free(a);
#endif

	return a;
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

	Assert(bms_is_valid_set(a));

	/* do nothing if nothing is called for, without further checking */
	if (upper < lower)
	{
#ifdef REALLOCATE_BITMAPSETS
		a = bms_copy_and_free(a);
#endif

		return a;
	}

	if (lower < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) palloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->type = T_Bitmapset;
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
		i = oldnwords;
		do
		{
			a->words[i] = 0;
		} while (++i < a->nwords);
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

#ifdef REALLOCATE_BITMAPSETS

	/*
	 * There's no guarantee that the repalloc returned a new pointer, so copy
	 * and free unconditionally here.
	 */
	a = bms_copy_and_free(a);
#endif

	return a;
}

/*
 * bms_int_members - like bms_intersect, but left input is recycled when
 * possible
 */
Bitmapset *
bms_int_members(Bitmapset *a, const Bitmapset *b)
{
	int			lastnonzero;
	int			shortlen;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

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
	lastnonzero = -1;
	i = 0;
	do
	{
		a->words[i] &= b->words[i];

		if (a->words[i] != 0)
			lastnonzero = i;
	} while (++i < shortlen);

	/* If we computed an empty result, we must return NULL */
	if (lastnonzero == -1)
	{
		pfree(a);
		return NULL;
	}

	/* get rid of trailing zero words */
	a->nwords = lastnonzero + 1;

#ifdef REALLOCATE_BITMAPSETS
	a = bms_copy_and_free(a);
#endif

	return a;
}

/*
 * bms_del_members - delete members in 'a' that are set in 'b'.  'a' is
 * recycled when possible.
 */
Bitmapset *
bms_del_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
	{
#ifdef REALLOCATE_BITMAPSETS
		a = bms_copy_and_free(a);
#endif

		return a;
	}

	/* Remove b's bits from a; we need never copy */
	if (a->nwords > b->nwords)
	{
		/*
		 * We'll never need to remove trailing zero words when 'a' has more
		 * words than 'b'.
		 */
		i = 0;
		do
		{
			a->words[i] &= ~b->words[i];
		} while (++i < b->nwords);
	}
	else
	{
		int			lastnonzero = -1;

		/* we may need to remove trailing zero words from the result. */
		i = 0;
		do
		{
			a->words[i] &= ~b->words[i];

			/* remember the last non-zero word */
			if (a->words[i] != 0)
				lastnonzero = i;
		} while (++i < a->nwords);

		/* check if 'a' has become empty */
		if (lastnonzero == -1)
		{
			pfree(a);
			return NULL;
		}

		/* trim off any trailing zero words */
		a->nwords = lastnonzero + 1;
	}

#ifdef REALLOCATE_BITMAPSETS
	a = bms_copy_and_free(a);
#endif

	return a;
}

/*
 * bms_join - like bms_union, but *either* input *may* be recycled
 */
Bitmapset *
bms_join(Bitmapset *a, Bitmapset *b)
{
	Bitmapset  *result;
	Bitmapset  *other;
	int			otherlen;
	int			i;

	Assert(bms_is_valid_set(a));
	Assert(bms_is_valid_set(b));

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
#ifdef REALLOCATE_BITMAPSETS
		b = bms_copy_and_free(b);
#endif

		return b;
	}
	if (b == NULL)
	{
#ifdef REALLOCATE_BITMAPSETS
		a = bms_copy_and_free(a);
#endif

		return a;
	}

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
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	if (other != result)		/* pure paranoia */
		pfree(other);

#ifdef REALLOCATE_BITMAPSETS
	result = bms_copy_and_free(result);
#endif

	return result;
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

	Assert(bms_is_valid_set(a));

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

	Assert(bms_is_valid_set(a));

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
 */
uint32
bms_hash_value(const Bitmapset *a)
{
	Assert(bms_is_valid_set(a));

	if (a == NULL)
		return 0;				/* All empty sets hash to 0 */
	return DatumGetUInt32(hash_any((const unsigned char *) a->words,
								   a->nwords * sizeof(bitmapword)));
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
