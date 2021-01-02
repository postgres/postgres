/*-------------------------------------------------------------------------
 * unicode_norm.c
 *		Normalize a Unicode string
 *
 * This implements Unicode normalization, per the documentation at
 * https://www.unicode.org/reports/tr15/.
 *
 * Portions Copyright (c) 2017-2021, PostgreSQL Global Development Group
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
#ifndef FRONTEND
#include "common/unicode_norm_hashfunc.h"
#include "common/unicode_normprops_table.h"
#include "port/pg_bswap.h"
#else
#include "common/unicode_norm_table.h"
#endif

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

#ifdef FRONTEND
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

#endif

/*
 * get_code_entry
 *
 * Get the entry corresponding to code in the decomposition lookup table.
 * The backend version of this code uses a perfect hash function for the
 * lookup, while the frontend version uses a binary search.
 */
static const pg_unicode_decomposition *
get_code_entry(pg_wchar code)
{
#ifndef FRONTEND
	int			h;
	uint32		hashkey;
	pg_unicode_decompinfo decompinfo = UnicodeDecompInfo;

	/*
	 * Compute the hash function. The hash key is the codepoint with the bytes
	 * in network order.
	 */
	hashkey = pg_hton32(code);
	h = decompinfo.hash(&hashkey);

	/* An out-of-range result implies no match */
	if (h < 0 || h >= decompinfo.num_decomps)
		return NULL;

	/*
	 * Since it's a perfect hash, we need only match to the specific codepoint
	 * it identifies.
	 */
	if (code != decompinfo.decomps[h].codepoint)
		return NULL;

	/* Success! */
	return &decompinfo.decomps[h];
#else
	return bsearch(&(code),
				   UnicodeDecompMain,
				   lengthof(UnicodeDecompMain),
				   sizeof(pg_unicode_decomposition),
				   conv_compare);
#endif
}

/*
 * Get the combining class of the given codepoint.
 */
static uint8
get_canonical_class(pg_wchar code)
{
	const pg_unicode_decomposition *entry = get_code_entry(code);

	/*
	 * If no entries are found, the character used is either an Hangul
	 * character or a character with a class of 0 and no decompositions.
	 */
	if (!entry)
		return 0;
	else
		return entry->comb_class;
}

/*
 * Given a decomposition entry looked up earlier, get the decomposed
 * characters.
 *
 * Note: the returned pointer can point to statically allocated buffer, and
 * is only valid until next call to this function!
 */
static const pg_wchar *
get_code_decomposition(const pg_unicode_decomposition *entry, int *dec_size)
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
get_decomposed_size(pg_wchar code, bool compat)
{
	const pg_unicode_decomposition *entry;
	int			size = 0;
	int			i;
	const uint32 *decomp;
	int			dec_size;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory as
	 * decomposition is algorithmic. See
	 * https://www.unicode.org/reports/tr15/tr15-18.html, annex 10 for details
	 * on the matter.
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
	if (entry == NULL || DECOMPOSITION_SIZE(entry) == 0 ||
		(!compat && DECOMPOSITION_IS_COMPAT(entry)))
		return 1;

	/*
	 * If this entry has other decomposition codes look at them as well. First
	 * get its decomposition in the list of tables available.
	 */
	decomp = get_code_decomposition(entry, &dec_size);
	for (i = 0; i < dec_size; i++)
	{
		uint32		lcode = decomp[i];

		size += get_decomposed_size(lcode, compat);
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
		/* make syllable of form LVT */
		uint32		tindex = code - TBASE;

		*result = start + tindex;
		return true;
	}
	else
	{
		const pg_unicode_decomposition *entry;

		/*
		 * Do an inverse lookup of the decomposition tables to see if anything
		 * matches. The comparison just needs to be a perfect match on the
		 * sub-table of size two, because the start character has already been
		 * recomposed partially.  This lookup uses a perfect hash function for
		 * the backend code.
		 */
#ifndef FRONTEND

		int			h,
					inv_lookup_index;
		uint64		hashkey;
		pg_unicode_recompinfo recompinfo = UnicodeRecompInfo;

		/*
		 * Compute the hash function. The hash key is formed by concatenating
		 * bytes of the two codepoints in network order. See also
		 * src/common/unicode/generate-unicode_norm_table.pl.
		 */
		hashkey = pg_hton64(((uint64) start << 32) | (uint64) code);
		h = recompinfo.hash(&hashkey);

		/* An out-of-range result implies no match */
		if (h < 0 || h >= recompinfo.num_recomps)
			return false;

		inv_lookup_index = recompinfo.inverse_lookup[h];
		entry = &UnicodeDecompMain[inv_lookup_index];

		if (start == UnicodeDecomp_codepoints[entry->dec_index] &&
			code == UnicodeDecomp_codepoints[entry->dec_index + 1])
		{
			*result = entry->codepoint;
			return true;
		}

#else

		int			i;

		for (i = 0; i < lengthof(UnicodeDecompMain); i++)
		{
			entry = &UnicodeDecompMain[i];

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
#endif							/* !FRONTEND */
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
decompose_code(pg_wchar code, bool compat, pg_wchar **result, int *current)
{
	const pg_unicode_decomposition *entry;
	int			i;
	const uint32 *decomp;
	int			dec_size;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory as
	 * decomposition is algorithmic. See
	 * https://www.unicode.org/reports/tr15/tr15-18.html, annex 10 for details
	 * on the matter.
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
	if (entry == NULL || DECOMPOSITION_SIZE(entry) == 0 ||
		(!compat && DECOMPOSITION_IS_COMPAT(entry)))
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
		decompose_code(lcode, compat, result, current);
	}
}

/*
 * unicode_normalize - Normalize a Unicode string to the specified form.
 *
 * The input is a 0-terminated array of codepoints.
 *
 * In frontend, returns a 0-terminated array of codepoints, allocated with
 * malloc. Or NULL if we run out of memory. In backend, the returned
 * string is palloc'd instead, and OOM is reported with ereport().
 */
pg_wchar *
unicode_normalize(UnicodeNormalizationForm form, const pg_wchar *input)
{
	bool		compat = (form == UNICODE_NFKC || form == UNICODE_NFKD);
	bool		recompose = (form == UNICODE_NFC || form == UNICODE_NFKC);
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
		decomp_size += get_decomposed_size(*p, compat);

	decomp_chars = (pg_wchar *) ALLOC((decomp_size + 1) * sizeof(pg_wchar));
	if (decomp_chars == NULL)
		return NULL;

	/*
	 * Now fill in each entry recursively. This needs a second pass on the
	 * decomposition table.
	 */
	current_size = 0;
	for (p = input; *p; p++)
		decompose_code(*p, compat, &decomp_chars, &current_size);
	decomp_chars[decomp_size] = '\0';
	Assert(decomp_size == current_size);

	/*
	 * Now apply canonical ordering.
	 */
	for (count = 1; count < decomp_size; count++)
	{
		pg_wchar	prev = decomp_chars[count - 1];
		pg_wchar	next = decomp_chars[count];
		pg_wchar	tmp;
		const uint8 prevClass = get_canonical_class(prev);
		const uint8 nextClass = get_canonical_class(next);

		/*
		 * Per Unicode (https://www.unicode.org/reports/tr15/tr15-18.html)
		 * annex 4, a sequence of two adjacent characters in a string is an
		 * exchangeable pair if the combining class (from the Unicode
		 * Character Database) for the first character is greater than the
		 * combining class for the second, and the second is not a starter.  A
		 * character is a starter if its combining class is 0.
		 */
		if (prevClass == 0 || nextClass == 0)
			continue;

		if (prevClass <= nextClass)
			continue;

		/* exchange can happen */
		tmp = decomp_chars[count - 1];
		decomp_chars[count - 1] = decomp_chars[count];
		decomp_chars[count] = tmp;

		/* backtrack to check again */
		if (count > 1)
			count -= 2;
	}

	if (!recompose)
		return decomp_chars;

	/*
	 * The last phase of NFC and NFKC is the recomposition of the reordered
	 * Unicode string using combining classes. The recomposed string cannot be
	 * longer than the decomposed one, so make the allocation of the output
	 * string based on that assumption.
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
		int			ch_class = get_canonical_class(ch);
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

/*
 * Normalization "quick check" algorithm; see
 * <http://www.unicode.org/reports/tr15/#Detecting_Normalization_Forms>
 */

/* We only need this in the backend. */
#ifndef FRONTEND

static const pg_unicode_normprops *
qc_hash_lookup(pg_wchar ch, const pg_unicode_norminfo *norminfo)
{
	int			h;
	uint32		hashkey;

	/*
	 * Compute the hash function. The hash key is the codepoint with the bytes
	 * in network order.
	 */
	hashkey = pg_hton32(ch);
	h = norminfo->hash(&hashkey);

	/* An out-of-range result implies no match */
	if (h < 0 || h >= norminfo->num_normprops)
		return NULL;

	/*
	 * Since it's a perfect hash, we need only match to the specific codepoint
	 * it identifies.
	 */
	if (ch != norminfo->normprops[h].codepoint)
		return NULL;

	/* Success! */
	return &norminfo->normprops[h];
}

/*
 * Look up the normalization quick check character property
 */
static UnicodeNormalizationQC
qc_is_allowed(UnicodeNormalizationForm form, pg_wchar ch)
{
	const pg_unicode_normprops *found = NULL;

	switch (form)
	{
		case UNICODE_NFC:
			found = qc_hash_lookup(ch, &UnicodeNormInfo_NFC_QC);
			break;
		case UNICODE_NFKC:
			found = qc_hash_lookup(ch, &UnicodeNormInfo_NFKC_QC);
			break;
		default:
			Assert(false);
			break;
	}

	if (found)
		return found->quickcheck;
	else
		return UNICODE_NORM_QC_YES;
}

UnicodeNormalizationQC
unicode_is_normalized_quickcheck(UnicodeNormalizationForm form, const pg_wchar *input)
{
	uint8		lastCanonicalClass = 0;
	UnicodeNormalizationQC result = UNICODE_NORM_QC_YES;

	/*
	 * For the "D" forms, we don't run the quickcheck.  We don't include the
	 * lookup tables for those because they are huge, checking for these
	 * particular forms is less common, and running the slow path is faster
	 * for the "D" forms than the "C" forms because you don't need to
	 * recompose, which is slow.
	 */
	if (form == UNICODE_NFD || form == UNICODE_NFKD)
		return UNICODE_NORM_QC_MAYBE;

	for (const pg_wchar *p = input; *p; p++)
	{
		pg_wchar	ch = *p;
		uint8		canonicalClass;
		UnicodeNormalizationQC check;

		canonicalClass = get_canonical_class(ch);
		if (lastCanonicalClass > canonicalClass && canonicalClass != 0)
			return UNICODE_NORM_QC_NO;

		check = qc_is_allowed(form, ch);
		if (check == UNICODE_NORM_QC_NO)
			return UNICODE_NORM_QC_NO;
		else if (check == UNICODE_NORM_QC_MAYBE)
			result = UNICODE_NORM_QC_MAYBE;

		lastCanonicalClass = canonicalClass;
	}
	return result;
}

#endif							/* !FRONTEND */
