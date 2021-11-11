/*-------------------------------------------------------------------------
 * unicode_norm.c
 *		Normalize a Unicode string to NFKC form
 *
 * This implements Unicode normalization, per the documentation at
 * http://www.unicode.org/reports/tr15/.
 *
 * Portions Copyright (c) 2017-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode_norm.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/unicode_norm.h"
#include "common/unicode_norm_table.h"

#ifndef FRONTEND
#define ALLOC(size) palloc(size)
#define FREE(size) pfree(size)
#else
#define ALLOC(size) malloc(size)
#define FREE(size) free(size)
#endif

/* Constants for calculations with Hangul characters */
#define SBASE		0xAC00		/* U+AC00 */
#define LBASE		0x1100		/* U+1100 */
#define VBASE		0x1161		/* U+1161 */
#define TBASE		0x11A7		/* U+11A7 */
#define LCOUNT		19
#define VCOUNT		21
#define TCOUNT		28
#define NCOUNT		VCOUNT * TCOUNT
#define SCOUNT		LCOUNT * NCOUNT

/* comparison routine for bsearch() of decomposition lookup table. */
static int
conv_compare(const void *p1, const void *p2)
{
	uint32		v1,
				v2;

	v1 = *(const uint32 *) p1;
	v2 = ((const pg_unicode_decomposition *) p2)->codepoint;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * Get the entry corresponding to code in the decomposition lookup table.
 */
static pg_unicode_decomposition *
get_code_entry(pg_wchar code)
{
	return bsearch(&(code),
				   UnicodeDecompMain,
				   lengthof(UnicodeDecompMain),
				   sizeof(pg_unicode_decomposition),
				   conv_compare);
}

/*
 * Given a decomposition entry looked up earlier, get the decomposed
 * characters.
 *
 * Note: the returned pointer can point to statically allocated buffer, and
 * is only valid until next call to this function!
 */
static const pg_wchar *
get_code_decomposition(pg_unicode_decomposition *entry, int *dec_size)
{
	static pg_wchar x;

	if (DECOMPOSITION_IS_INLINE(entry))
	{
		Assert(DECOMPOSITION_SIZE(entry) == 1);
		x = (pg_wchar) entry->dec_index;
		*dec_size = 1;
		return &x;
	}
	else
	{
		*dec_size = DECOMPOSITION_SIZE(entry);
		return &UnicodeDecomp_codepoints[entry->dec_index];
	}
}

/*
 * Calculate how many characters a given character will decompose to.
 *
 * This needs to recurse, if the character decomposes into characters that
 * are, in turn, decomposable.
 */
static int
get_decomposed_size(pg_wchar code)
{
	pg_unicode_decomposition *entry;
	int			size = 0;
	int			i;
	const uint32 *decomp;
	int			dec_size;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory as
	 * decomposition is algorithmic. See
	 * http://unicode.org/reports/tr15/tr15-18.html, annex 10 for details on
	 * the matter.
	 */
	if (code >= SBASE && code < SBASE + SCOUNT)
	{
		uint32		tindex,
					sindex;

		sindex = code - SBASE;
		tindex = sindex % TCOUNT;

		if (tindex != 0)
			return 3;
		return 2;
	}

	entry = get_code_entry(code);

	/*
	 * Just count current code if no other decompositions.  A NULL entry is
	 * equivalent to a character with class 0 and no decompositions.
	 */
	if (entry == NULL || DECOMPOSITION_SIZE(entry) == 0)
		return 1;

	/*
	 * If this entry has other decomposition codes look at them as well. First
	 * get its decomposition in the list of tables available.
	 */
	decomp = get_code_decomposition(entry, &dec_size);
	for (i = 0; i < dec_size; i++)
	{
		uint32		lcode = decomp[i];

		size += get_decomposed_size(lcode);
	}

	return size;
}

/*
 * Recompose a set of characters. For hangul characters, the calculation
 * is algorithmic. For others, an inverse lookup at the decomposition
 * table is necessary. Returns true if a recomposition can be done, and
 * false otherwise.
 */
static bool
recompose_code(uint32 start, uint32 code, uint32 *result)
{
	/*
	 * Handle Hangul characters algorithmically, per the Unicode spec.
	 *
	 * Check if two current characters are L and V.
	 */
	if (start >= LBASE && start < LBASE + LCOUNT &&
		code >= VBASE && code < VBASE + VCOUNT)
	{
		/* make syllable of form LV */
		uint32		lindex = start - LBASE;
		uint32		vindex = code - VBASE;

		*result = SBASE + (lindex * VCOUNT + vindex) * TCOUNT;
		return true;
	}
	/* Check if two current characters are LV and T */
	else if (start >= SBASE && start < (SBASE + SCOUNT) &&
			 ((start - SBASE) % TCOUNT) == 0 &&
			 code >= TBASE && code < (TBASE + TCOUNT))
	{
		/* make syllable of from LVT */
		uint32		tindex = code - TBASE;

		*result = start + tindex;
		return true;
	}
	else
	{
		int			i;

		/*
		 * Do an inverse lookup of the decomposition tables to see if anything
		 * matches. The comparison just needs to be a perfect match on the
		 * sub-table of size two, because the start character has already been
		 * recomposed partially.
		 */
		for (i = 0; i < lengthof(UnicodeDecompMain); i++)
		{
			const pg_unicode_decomposition *entry = &UnicodeDecompMain[i];

			if (DECOMPOSITION_SIZE(entry) != 2)
				continue;

			if (DECOMPOSITION_NO_COMPOSE(entry))
				continue;

			if (start == UnicodeDecomp_codepoints[entry->dec_index] &&
				code == UnicodeDecomp_codepoints[entry->dec_index + 1])
			{
				*result = entry->codepoint;
				return true;
			}
		}
	}

	return false;
}

/*
 * Decompose the given code into the array given by caller. The
 * decomposition begins at the position given by caller, saving one
 * lookup on the decomposition table. The current position needs to be
 * updated here to let the caller know from where to continue filling
 * in the array result.
 */
static void
decompose_code(pg_wchar code, pg_wchar **result, int *current)
{
	pg_unicode_decomposition *entry;
	int			i;
	const uint32 *decomp;
	int			dec_size;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory as
	 * decomposition is algorithmic. See
	 * http://unicode.org/reports/tr15/tr15-18.html, annex 10 for details on
	 * the matter.
	 */
	if (code >= SBASE && code < SBASE + SCOUNT)
	{
		uint32		l,
					v,
					tindex,
					sindex;
		pg_wchar   *res = *result;

		sindex = code - SBASE;
		l = LBASE + sindex / (VCOUNT * TCOUNT);
		v = VBASE + (sindex % (VCOUNT * TCOUNT)) / TCOUNT;
		tindex = sindex % TCOUNT;

		res[*current] = l;
		(*current)++;
		res[*current] = v;
		(*current)++;

		if (tindex != 0)
		{
			res[*current] = TBASE + tindex;
			(*current)++;
		}

		return;
	}

	entry = get_code_entry(code);

	/*
	 * Just fill in with the current decomposition if there are no
	 * decomposition codes to recurse to.  A NULL entry is equivalent to a
	 * character with class 0 and no decompositions, so just leave also in
	 * this case.
	 */
	if (entry == NULL || DECOMPOSITION_SIZE(entry) == 0)
	{
		pg_wchar   *res = *result;

		res[*current] = code;
		(*current)++;
		return;
	}

	/*
	 * If this entry has other decomposition codes look at them as well.
	 */
	decomp = get_code_decomposition(entry, &dec_size);
	for (i = 0; i < dec_size; i++)
	{
		pg_wchar	lcode = (pg_wchar) decomp[i];

		/* Leave if no more decompositions */
		decompose_code(lcode, result, current);
	}
}

/*
 * unicode_normalize_kc - Normalize a Unicode string to NFKC form.
 *
 * The input is a 0-terminated array of codepoints.
 *
 * In frontend, returns a 0-terminated array of codepoints, allocated with
 * malloc. Or NULL if we run out of memory. In backend, the returned
 * string is palloc'd instead, and OOM is reported with ereport().
 */
pg_wchar *
unicode_normalize_kc(const pg_wchar *input)
{
	pg_wchar   *decomp_chars;
	pg_wchar   *recomp_chars;
	int			decomp_size,
				current_size;
	int			count;
	const pg_wchar *p;

	/* variables for recomposition */
	int			last_class;
	int			starter_pos;
	int			target_pos;
	uint32		starter_ch;

	/* First, do character decomposition */

	/*
	 * Calculate how many characters long the decomposed version will be.
	 */
	decomp_size = 0;
	for (p = input; *p; p++)
		decomp_size += get_decomposed_size(*p);

	decomp_chars = (pg_wchar *) ALLOC((decomp_size + 1) * sizeof(pg_wchar));
	if (decomp_chars == NULL)
		return NULL;

	/*
	 * Now fill in each entry recursively. This needs a second pass on the
	 * decomposition table.
	 */
	current_size = 0;
	for (p = input; *p; p++)
		decompose_code(*p, &decomp_chars, &current_size);
	decomp_chars[decomp_size] = '\0';
	Assert(decomp_size == current_size);

	/* Leave if there is nothing to decompose */
	if (decomp_size == 0)
		return decomp_chars;

	/*
	 * Now apply canonical ordering.
	 */
	for (count = 1; count < decomp_size; count++)
	{
		pg_wchar	prev = decomp_chars[count - 1];
		pg_wchar	next = decomp_chars[count];
		pg_wchar	tmp;
		pg_unicode_decomposition *prevEntry = get_code_entry(prev);
		pg_unicode_decomposition *nextEntry = get_code_entry(next);

		/*
		 * If no entries are found, the character used is either an Hangul
		 * character or a character with a class of 0 and no decompositions,
		 * so move to next result.
		 */
		if (prevEntry == NULL || nextEntry == NULL)
			continue;

		/*
		 * Per Unicode (http://unicode.org/reports/tr15/tr15-18.html) annex 4,
		 * a sequence of two adjacent characters in a string is an
		 * exchangeable pair if the combining class (from the Unicode
		 * Character Database) for the first character is greater than the
		 * combining class for the second, and the second is not a starter.  A
		 * character is a starter if its combining class is 0.
		 */
		if (nextEntry->comb_class == 0x0 || prevEntry->comb_class == 0x0)
			continue;

		if (prevEntry->comb_class <= nextEntry->comb_class)
			continue;

		/* exchange can happen */
		tmp = decomp_chars[count - 1];
		decomp_chars[count - 1] = decomp_chars[count];
		decomp_chars[count] = tmp;

		/* backtrack to check again */
		if (count > 1)
			count -= 2;
	}

	/*
	 * The last phase of NFKC is the recomposition of the reordered Unicode
	 * string using combining classes. The recomposed string cannot be longer
	 * than the decomposed one, so make the allocation of the output string
	 * based on that assumption.
	 */
	recomp_chars = (pg_wchar *) ALLOC((decomp_size + 1) * sizeof(pg_wchar));
	if (!recomp_chars)
	{
		FREE(decomp_chars);
		return NULL;
	}

	last_class = -1;			/* this eliminates a special check */
	starter_pos = 0;
	target_pos = 1;
	starter_ch = recomp_chars[0] = decomp_chars[0];

	for (count = 1; count < decomp_size; count++)
	{
		pg_wchar	ch = decomp_chars[count];
		pg_unicode_decomposition *ch_entry = get_code_entry(ch);
		int			ch_class = (ch_entry == NULL) ? 0 : ch_entry->comb_class;
		pg_wchar	composite;

		if (last_class < ch_class &&
			recompose_code(starter_ch, ch, &composite))
		{
			recomp_chars[starter_pos] = composite;
			starter_ch = composite;
		}
		else if (ch_class == 0)
		{
			starter_pos = target_pos;
			starter_ch = ch;
			last_class = -1;
			recomp_chars[target_pos++] = ch;
		}
		else
		{
			last_class = ch_class;
			recomp_chars[target_pos++] = ch;
		}
	}
	recomp_chars[target_pos] = (pg_wchar) '\0';

	FREE(decomp_chars);

	return recomp_chars;
}
