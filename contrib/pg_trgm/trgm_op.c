/*
 * contrib/pg_trgm/trgm_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "lib/qunique.h"
#include "trgm.h"
#include "tsearch/ts_locale.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"

PG_MODULE_MAGIC;

/* GUC variables */
double		similarity_threshold = 0.3f;
double		word_similarity_threshold = 0.6f;
double		strict_word_similarity_threshold = 0.5f;

void		_PG_init(void);

PG_FUNCTION_INFO_V1(set_limit);
PG_FUNCTION_INFO_V1(show_limit);
PG_FUNCTION_INFO_V1(show_trgm);
PG_FUNCTION_INFO_V1(similarity);
PG_FUNCTION_INFO_V1(word_similarity);
PG_FUNCTION_INFO_V1(strict_word_similarity);
PG_FUNCTION_INFO_V1(similarity_dist);
PG_FUNCTION_INFO_V1(similarity_op);
PG_FUNCTION_INFO_V1(word_similarity_op);
PG_FUNCTION_INFO_V1(word_similarity_commutator_op);
PG_FUNCTION_INFO_V1(word_similarity_dist_op);
PG_FUNCTION_INFO_V1(word_similarity_dist_commutator_op);
PG_FUNCTION_INFO_V1(strict_word_similarity_op);
PG_FUNCTION_INFO_V1(strict_word_similarity_commutator_op);
PG_FUNCTION_INFO_V1(strict_word_similarity_dist_op);
PG_FUNCTION_INFO_V1(strict_word_similarity_dist_commutator_op);

/* Trigram with position */
typedef struct
{
	trgm		trg;
	int			index;
} pos_trgm;

/* Trigram bound type */
typedef uint8 TrgmBound;
#define TRGM_BOUND_LEFT				0x01	/* trigram is left bound of word */
#define TRGM_BOUND_RIGHT			0x02	/* trigram is right bound of word */

/* Word similarity flags */
#define WORD_SIMILARITY_CHECK_ONLY	0x01	/* only check existence of similar
											 * search pattern in text */
#define WORD_SIMILARITY_STRICT		0x02	/* force bounds of extent to match
											 * word bounds */

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomRealVariable("pg_trgm.similarity_threshold",
							 "Sets the threshold used by the % operator.",
							 "Valid range is 0.0 .. 1.0.",
							 &similarity_threshold,
							 0.3,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomRealVariable("pg_trgm.word_similarity_threshold",
							 "Sets the threshold used by the <% operator.",
							 "Valid range is 0.0 .. 1.0.",
							 &word_similarity_threshold,
							 0.6,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomRealVariable("pg_trgm.strict_word_similarity_threshold",
							 "Sets the threshold used by the <<% operator.",
							 "Valid range is 0.0 .. 1.0.",
							 &strict_word_similarity_threshold,
							 0.5,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_trgm");
}

/*
 * Deprecated function.
 * Use "pg_trgm.similarity_threshold" GUC variable instead of this function.
 */
Datum
set_limit(PG_FUNCTION_ARGS)
{
	float4		nlimit = PG_GETARG_FLOAT4(0);
	char	   *nlimit_str;
	Oid			func_out_oid;
	bool		is_varlena;

	getTypeOutputInfo(FLOAT4OID, &func_out_oid, &is_varlena);

	nlimit_str = OidOutputFunctionCall(func_out_oid, Float4GetDatum(nlimit));

	SetConfigOption("pg_trgm.similarity_threshold", nlimit_str,
					PGC_USERSET, PGC_S_SESSION);

	PG_RETURN_FLOAT4(similarity_threshold);
}


/*
 * Get similarity threshold for given index scan strategy number.
 */
double
index_strategy_get_limit(StrategyNumber strategy)
{
	switch (strategy)
	{
		case SimilarityStrategyNumber:
			return similarity_threshold;
		case WordSimilarityStrategyNumber:
			return word_similarity_threshold;
		case StrictWordSimilarityStrategyNumber:
			return strict_word_similarity_threshold;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			break;
	}

	return 0.0;					/* keep compiler quiet */
}

/*
 * Deprecated function.
 * Use "pg_trgm.similarity_threshold" GUC variable instead of this function.
 */
Datum
show_limit(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT4(similarity_threshold);
}

static int
comp_trgm(const void *a, const void *b)
{
	return CMPTRGM(a, b);
}

/*
 * Finds first word in string, returns pointer to the word,
 * endword points to the character after word
 */
static char *
find_word(char *str, int lenstr, char **endword, int *charlen)
{
	char	   *beginword = str;

	while (beginword - str < lenstr && !ISWORDCHR(beginword))
		beginword += pg_mblen(beginword);

	if (beginword - str >= lenstr)
		return NULL;

	*endword = beginword;
	*charlen = 0;
	while (*endword - str < lenstr && ISWORDCHR(*endword))
	{
		*endword += pg_mblen(*endword);
		(*charlen)++;
	}

	return beginword;
}

/*
 * Reduce a trigram (three possibly multi-byte characters) to a trgm,
 * which is always exactly three bytes.  If we have three single-byte
 * characters, we just use them as-is; otherwise we form a hash value.
 */
void
compact_trigram(trgm *tptr, char *str, int bytelen)
{
	if (bytelen == 3)
	{
		CPTRGM(tptr, str);
	}
	else
	{
		pg_crc32	crc;

		INIT_LEGACY_CRC32(crc);
		COMP_LEGACY_CRC32(crc, str, bytelen);
		FIN_LEGACY_CRC32(crc);

		/*
		 * use only 3 upper bytes from crc, hope, it's good enough hashing
		 */
		CPTRGM(tptr, &crc);
	}
}

/*
 * Adds trigrams from words (already padded).
 */
static trgm *
make_trigrams(trgm *tptr, char *str, int bytelen, int charlen)
{
	char	   *ptr = str;

	if (charlen < 3)
		return tptr;

	if (bytelen > charlen)
	{
		/* Find multibyte character boundaries and apply compact_trigram */
		int			lenfirst = pg_mblen(str),
					lenmiddle = pg_mblen(str + lenfirst),
					lenlast = pg_mblen(str + lenfirst + lenmiddle);

		while ((ptr - str) + lenfirst + lenmiddle + lenlast <= bytelen)
		{
			compact_trigram(tptr, ptr, lenfirst + lenmiddle + lenlast);

			ptr += lenfirst;
			tptr++;

			lenfirst = lenmiddle;
			lenmiddle = lenlast;
			lenlast = pg_mblen(ptr + lenfirst + lenmiddle);
		}
	}
	else
	{
		/* Fast path when there are no multibyte characters */
		Assert(bytelen == charlen);

		while (ptr - str < bytelen - 2 /* number of trigrams = strlen - 2 */ )
		{
			CPTRGM(tptr, ptr);
			ptr++;
			tptr++;
		}
	}

	return tptr;
}

/*
 * Make array of trigrams without sorting and removing duplicate items.
 *
 * trg: where to return the array of trigrams.
 * str: source string, of length slen bytes.
 * bounds: where to return bounds of trigrams (if needed).
 *
 * Returns length of the generated array.
 */
static int
generate_trgm_only(trgm *trg, char *str, int slen, TrgmBound *bounds)
{
	trgm	   *tptr;
	char	   *buf;
	int			charlen,
				bytelen;
	char	   *bword,
			   *eword;

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return 0;

	tptr = trg;

	/* Allocate a buffer for case-folded, blank-padded words */
	buf = (char *) palloc(slen * pg_database_encoding_max_length() + 4);

	if (LPADDING > 0)
	{
		*buf = ' ';
		if (LPADDING > 1)
			*(buf + 1) = ' ';
	}

	eword = str;
	while ((bword = find_word(eword, slen - (eword - str), &eword, &charlen)) != NULL)
	{
#ifdef IGNORECASE
		bword = lowerstr_with_len(bword, eword - bword);
		bytelen = strlen(bword);
#else
		bytelen = eword - bword;
#endif

		memcpy(buf + LPADDING, bword, bytelen);

#ifdef IGNORECASE
		pfree(bword);
#endif

		buf[LPADDING + bytelen] = ' ';
		buf[LPADDING + bytelen + 1] = ' ';

		/* Calculate trigrams marking their bounds if needed */
		if (bounds)
			bounds[tptr - trg] |= TRGM_BOUND_LEFT;
		tptr = make_trigrams(tptr, buf, bytelen + LPADDING + RPADDING,
							 charlen + LPADDING + RPADDING);
		if (bounds)
			bounds[tptr - trg - 1] |= TRGM_BOUND_RIGHT;
	}

	pfree(buf);

	return tptr - trg;
}

/*
 * Guard against possible overflow in the palloc requests below.  (We
 * don't worry about the additive constants, since palloc can detect
 * requests that are a little above MaxAllocSize --- we just need to
 * prevent integer overflow in the multiplications.)
 */
static void
protect_out_of_mem(int slen)
{
	if ((Size) (slen / 2) >= (MaxAllocSize / (sizeof(trgm) * 3)) ||
		(Size) slen >= (MaxAllocSize / pg_database_encoding_max_length()))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory")));
}

/*
 * Make array of trigrams with sorting and removing duplicate items.
 *
 * str: source string, of length slen bytes.
 *
 * Returns the sorted array of unique trigrams.
 */
TRGM *
generate_trgm(char *str, int slen)
{
	TRGM	   *trg;
	int			len;

	protect_out_of_mem(slen);

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) * 3);
	trg->flag = ARRKEY;

	len = generate_trgm_only(GETARR(trg), str, slen, NULL);
	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, len));

	if (len == 0)
		return trg;

	/*
	 * Make trigrams unique.
	 */
	if (len > 1)
	{
		qsort((void *) GETARR(trg), len, sizeof(trgm), comp_trgm);
		len = qunique(GETARR(trg), len, sizeof(trgm), comp_trgm);
	}

	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, len));

	return trg;
}

/*
 * Make array of positional trigrams from two trigram arrays trg1 and trg2.
 *
 * trg1: trigram array of search pattern, of length len1. trg1 is required
 *		 word which positions don't matter and replaced with -1.
 * trg2: trigram array of text, of length len2. trg2 is haystack where we
 *		 search and have to store its positions.
 *
 * Returns concatenated trigram array.
 */
static pos_trgm *
make_positional_trgm(trgm *trg1, int len1, trgm *trg2, int len2)
{
	pos_trgm   *result;
	int			i,
				len = len1 + len2;

	result = (pos_trgm *) palloc(sizeof(pos_trgm) * len);

	for (i = 0; i < len1; i++)
	{
		memcpy(&result[i].trg, &trg1[i], sizeof(trgm));
		result[i].index = -1;
	}

	for (i = 0; i < len2; i++)
	{
		memcpy(&result[i + len1].trg, &trg2[i], sizeof(trgm));
		result[i + len1].index = i;
	}

	return result;
}

/*
 * Compare position trigrams: compare trigrams first and position second.
 */
static int
comp_ptrgm(const void *v1, const void *v2)
{
	const pos_trgm *p1 = (const pos_trgm *) v1;
	const pos_trgm *p2 = (const pos_trgm *) v2;
	int			cmp;

	cmp = CMPTRGM(p1->trg, p2->trg);
	if (cmp != 0)
		return cmp;

	if (p1->index < p2->index)
		return -1;
	else if (p1->index == p2->index)
		return 0;
	else
		return 1;
}

/*
 * Iterative search function which calculates maximum similarity with word in
 * the string. But maximum similarity is calculated only if check_only == false.
 *
 * trg2indexes: array which stores indexes of the array "found".
 * found: array which stores true of false values.
 * ulen1: count of unique trigrams of array "trg1".
 * len2: length of array "trg2" and array "trg2indexes".
 * len: length of the array "found".
 * lags: set of boolean flags parameterizing similarity calculation.
 * bounds: whether each trigram is left/right bound of word.
 *
 * Returns word similarity.
 */
static float4
iterate_word_similarity(int *trg2indexes,
						bool *found,
						int ulen1,
						int len2,
						int len,
						uint8 flags,
						TrgmBound *bounds)
{
	int		   *lastpos,
				i,
				ulen2 = 0,
				count = 0,
				upper = -1,
				lower;
	float4		smlr_cur,
				smlr_max = 0.0f;
	double		threshold;

	Assert(bounds || !(flags & WORD_SIMILARITY_STRICT));

	/* Select appropriate threshold */
	threshold = (flags & WORD_SIMILARITY_STRICT) ?
		strict_word_similarity_threshold :
		word_similarity_threshold;

	/*
	 * Consider first trigram as initial lower bound for strict word
	 * similarity, or initialize it later with first trigram present for plain
	 * word similarity.
	 */
	lower = (flags & WORD_SIMILARITY_STRICT) ? 0 : -1;

	/* Memorise last position of each trigram */
	lastpos = (int *) palloc(sizeof(int) * len);
	memset(lastpos, -1, sizeof(int) * len);

	for (i = 0; i < len2; i++)
	{
		/* Get index of next trigram */
		int			trgindex = trg2indexes[i];

		/* Update last position of this trigram */
		if (lower >= 0 || found[trgindex])
		{
			if (lastpos[trgindex] < 0)
			{
				ulen2++;
				if (found[trgindex])
					count++;
			}
			lastpos[trgindex] = i;
		}

		/*
		 * Adjust upper bound if trigram is upper bound of word for strict
		 * word similarity, or if trigram is present in required substring for
		 * plain word similarity
		 */
		if ((flags & WORD_SIMILARITY_STRICT) ? (bounds[i] & TRGM_BOUND_RIGHT)
			: found[trgindex])
		{
			int			prev_lower,
						tmp_ulen2,
						tmp_lower,
						tmp_count;

			upper = i;
			if (lower == -1)
			{
				lower = i;
				ulen2 = 1;
			}

			smlr_cur = CALCSML(count, ulen1, ulen2);

			/* Also try to adjust lower bound for greater similarity */
			tmp_count = count;
			tmp_ulen2 = ulen2;
			prev_lower = lower;
			for (tmp_lower = lower; tmp_lower <= upper; tmp_lower++)
			{
				float		smlr_tmp;
				int			tmp_trgindex;

				/*
				 * Adjust lower bound only if trigram is lower bound of word
				 * for strict word similarity, or consider every trigram as
				 * lower bound for plain word similarity.
				 */
				if (!(flags & WORD_SIMILARITY_STRICT)
					|| (bounds[tmp_lower] & TRGM_BOUND_LEFT))
				{
					smlr_tmp = CALCSML(tmp_count, ulen1, tmp_ulen2);
					if (smlr_tmp > smlr_cur)
					{
						smlr_cur = smlr_tmp;
						ulen2 = tmp_ulen2;
						lower = tmp_lower;
						count = tmp_count;
					}

					/*
					 * If we only check that word similarity is greater than
					 * threshold we do not need to calculate a maximum
					 * similarity.
					 */
					if ((flags & WORD_SIMILARITY_CHECK_ONLY)
						&& smlr_cur >= threshold)
						break;
				}

				tmp_trgindex = trg2indexes[tmp_lower];
				if (lastpos[tmp_trgindex] == tmp_lower)
				{
					tmp_ulen2--;
					if (found[tmp_trgindex])
						tmp_count--;
				}
			}

			smlr_max = Max(smlr_max, smlr_cur);

			/*
			 * if we only check that word similarity is greater than threshold
			 * we do not need to calculate a maximum similarity.
			 */
			if ((flags & WORD_SIMILARITY_CHECK_ONLY) && smlr_max >= threshold)
				break;

			for (tmp_lower = prev_lower; tmp_lower < lower; tmp_lower++)
			{
				int			tmp_trgindex;

				tmp_trgindex = trg2indexes[tmp_lower];
				if (lastpos[tmp_trgindex] == tmp_lower)
					lastpos[tmp_trgindex] = -1;
			}
		}
	}

	pfree(lastpos);

	return smlr_max;
}

/*
 * Calculate word similarity.
 * This function prepare two arrays: "trg2indexes" and "found". Then this arrays
 * are used to calculate word similarity using iterate_word_similarity().
 *
 * "trg2indexes" is array which stores indexes of the array "found".
 * In other words:
 * trg2indexes[j] = i;
 * found[i] = true (or false);
 * If found[i] == true then there is trigram trg2[j] in array "trg1".
 * If found[i] == false then there is not trigram trg2[j] in array "trg1".
 *
 * str1: search pattern string, of length slen1 bytes.
 * str2: text in which we are looking for a word, of length slen2 bytes.
 * flags: set of boolean flags parameterizing similarity calculation.
 *
 * Returns word similarity.
 */
static float4
calc_word_similarity(char *str1, int slen1, char *str2, int slen2,
					 uint8 flags)
{
	bool	   *found;
	pos_trgm   *ptrg;
	trgm	   *trg1;
	trgm	   *trg2;
	int			len1,
				len2,
				len,
				i,
				j,
				ulen1;
	int		   *trg2indexes;
	float4		result;
	TrgmBound  *bounds;

	protect_out_of_mem(slen1 + slen2);

	/* Make positional trigrams */
	trg1 = (trgm *) palloc(sizeof(trgm) * (slen1 / 2 + 1) * 3);
	trg2 = (trgm *) palloc(sizeof(trgm) * (slen2 / 2 + 1) * 3);
	if (flags & WORD_SIMILARITY_STRICT)
		bounds = (TrgmBound *) palloc0(sizeof(TrgmBound) * (slen2 / 2 + 1) * 3);
	else
		bounds = NULL;

	len1 = generate_trgm_only(trg1, str1, slen1, NULL);
	len2 = generate_trgm_only(trg2, str2, slen2, bounds);

	ptrg = make_positional_trgm(trg1, len1, trg2, len2);
	len = len1 + len2;
	qsort(ptrg, len, sizeof(pos_trgm), comp_ptrgm);

	pfree(trg1);
	pfree(trg2);

	/*
	 * Merge positional trigrams array: enumerate each trigram and find its
	 * presence in required word.
	 */
	trg2indexes = (int *) palloc(sizeof(int) * len2);
	found = (bool *) palloc0(sizeof(bool) * len);

	ulen1 = 0;
	j = 0;
	for (i = 0; i < len; i++)
	{
		if (i > 0)
		{
			int			cmp = CMPTRGM(ptrg[i - 1].trg, ptrg[i].trg);

			if (cmp != 0)
			{
				if (found[j])
					ulen1++;
				j++;
			}
		}

		if (ptrg[i].index >= 0)
		{
			trg2indexes[ptrg[i].index] = j;
		}
		else
		{
			found[j] = true;
		}
	}
	if (found[j])
		ulen1++;

	/* Run iterative procedure to find maximum similarity with word */
	result = iterate_word_similarity(trg2indexes, found, ulen1, len2, len,
									 flags, bounds);

	pfree(trg2indexes);
	pfree(found);
	pfree(ptrg);

	return result;
}


/*
 * Extract the next non-wildcard part of a search string, i.e. a word bounded
 * by '_' or '%' meta-characters, non-word characters or string end.
 *
 * str: source string, of length lenstr bytes (need not be null-terminated)
 * buf: where to return the substring (must be long enough)
 * *bytelen: receives byte length of the found substring
 * *charlen: receives character length of the found substring
 *
 * Returns pointer to end+1 of the found substring in the source string.
 * Returns NULL if no word found (in which case buf, bytelen, charlen not set)
 *
 * If the found word is bounded by non-word characters or string boundaries
 * then this function will include corresponding padding spaces into buf.
 */
static const char *
get_wildcard_part(const char *str, int lenstr,
				  char *buf, int *bytelen, int *charlen)
{
	const char *beginword = str;
	const char *endword;
	char	   *s = buf;
	bool		in_leading_wildcard_meta = false;
	bool		in_trailing_wildcard_meta = false;
	bool		in_escape = false;
	int			clen;

	/*
	 * Find the first word character, remembering whether preceding character
	 * was wildcard meta-character.  Note that the in_escape state persists
	 * from this loop to the next one, since we may exit at a word character
	 * that is in_escape.
	 */
	while (beginword - str < lenstr)
	{
		if (in_escape)
		{
			if (ISWORDCHR(beginword))
				break;
			in_escape = false;
			in_leading_wildcard_meta = false;
		}
		else
		{
			if (ISESCAPECHAR(beginword))
				in_escape = true;
			else if (ISWILDCARDCHAR(beginword))
				in_leading_wildcard_meta = true;
			else if (ISWORDCHR(beginword))
				break;
			else
				in_leading_wildcard_meta = false;
		}
		beginword += pg_mblen(beginword);
	}

	/*
	 * Handle string end.
	 */
	if (beginword - str >= lenstr)
		return NULL;

	/*
	 * Add left padding spaces if preceding character wasn't wildcard
	 * meta-character.
	 */
	*charlen = 0;
	if (!in_leading_wildcard_meta)
	{
		if (LPADDING > 0)
		{
			*s++ = ' ';
			(*charlen)++;
			if (LPADDING > 1)
			{
				*s++ = ' ';
				(*charlen)++;
			}
		}
	}

	/*
	 * Copy data into buf until wildcard meta-character, non-word character or
	 * string boundary.  Strip escapes during copy.
	 */
	endword = beginword;
	while (endword - str < lenstr)
	{
		clen = pg_mblen(endword);
		if (in_escape)
		{
			if (ISWORDCHR(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
			{
				/*
				 * Back up endword to the escape character when stopping at an
				 * escaped char, so that subsequent get_wildcard_part will
				 * restart from the escape character.  We assume here that
				 * escape chars are single-byte.
				 */
				endword--;
				break;
			}
			in_escape = false;
		}
		else
		{
			if (ISESCAPECHAR(endword))
				in_escape = true;
			else if (ISWILDCARDCHAR(endword))
			{
				in_trailing_wildcard_meta = true;
				break;
			}
			else if (ISWORDCHR(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
				break;
		}
		endword += clen;
	}

	/*
	 * Add right padding spaces if next character isn't wildcard
	 * meta-character.
	 */
	if (!in_trailing_wildcard_meta)
	{
		if (RPADDING > 0)
		{
			*s++ = ' ';
			(*charlen)++;
			if (RPADDING > 1)
			{
				*s++ = ' ';
				(*charlen)++;
			}
		}
	}

	*bytelen = s - buf;
	return endword;
}

/*
 * Generates trigrams for wildcard search string.
 *
 * Returns array of trigrams that must occur in any string that matches the
 * wildcard string.  For example, given pattern "a%bcd%" the trigrams
 * " a", "bcd" would be extracted.
 */
TRGM *
generate_wildcard_trgm(const char *str, int slen)
{
	TRGM	   *trg;
	char	   *buf,
			   *buf2;
	trgm	   *tptr;
	int			len,
				charlen,
				bytelen;
	const char *eword;

	protect_out_of_mem(slen);

	trg = (TRGM *) palloc(TRGMHDRSIZE + sizeof(trgm) * (slen / 2 + 1) * 3);
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, TRGMHDRSIZE);

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return trg;

	tptr = GETARR(trg);

	/* Allocate a buffer for blank-padded, but not yet case-folded, words */
	buf = palloc(sizeof(char) * (slen + 4));

	/*
	 * Extract trigrams from each substring extracted by get_wildcard_part.
	 */
	eword = str;
	while ((eword = get_wildcard_part(eword, slen - (eword - str),
									  buf, &bytelen, &charlen)) != NULL)
	{
#ifdef IGNORECASE
		buf2 = lowerstr_with_len(buf, bytelen);
		bytelen = strlen(buf2);
#else
		buf2 = buf;
#endif

		/*
		 * count trigrams
		 */
		tptr = make_trigrams(tptr, buf2, bytelen, charlen);

#ifdef IGNORECASE
		pfree(buf2);
#endif
	}

	pfree(buf);

	if ((len = tptr - GETARR(trg)) == 0)
		return trg;

	/*
	 * Make trigrams unique.
	 */
	if (len > 1)
	{
		qsort((void *) GETARR(trg), len, sizeof(trgm), comp_trgm);
		len = qunique(GETARR(trg), len, sizeof(trgm), comp_trgm);
	}

	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, len));

	return trg;
}

uint32
trgm2int(trgm *ptr)
{
	uint32		val = 0;

	val |= *(((unsigned char *) ptr));
	val <<= 8;
	val |= *(((unsigned char *) ptr) + 1);
	val <<= 8;
	val |= *(((unsigned char *) ptr) + 2);

	return val;
}

Datum
show_trgm(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	TRGM	   *trg;
	Datum	   *d;
	ArrayType  *a;
	trgm	   *ptr;
	int			i;

	trg = generate_trgm(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in));
	d = (Datum *) palloc(sizeof(Datum) * (1 + ARRNELEM(trg)));

	for (i = 0, ptr = GETARR(trg); i < ARRNELEM(trg); i++, ptr++)
	{
		text	   *item = (text *) palloc(VARHDRSZ + Max(12, pg_database_encoding_max_length() * 3));

		if (pg_database_encoding_max_length() > 1 && !ISPRINTABLETRGM(ptr))
		{
			snprintf(VARDATA(item), 12, "0x%06x", trgm2int(ptr));
			SET_VARSIZE(item, VARHDRSZ + strlen(VARDATA(item)));
		}
		else
		{
			SET_VARSIZE(item, VARHDRSZ + 3);
			CPTRGM(VARDATA(item), ptr);
		}
		d[i] = PointerGetDatum(item);
	}

	a = construct_array(d,
						ARRNELEM(trg),
						TEXTOID,
						-1,
						false,
						TYPALIGN_INT);

	for (i = 0; i < ARRNELEM(trg); i++)
		pfree(DatumGetPointer(d[i]));

	pfree(d);
	pfree(trg);
	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_POINTER(a);
}

float4
cnt_sml(TRGM *trg1, TRGM *trg2, bool inexact)
{
	trgm	   *ptr1,
			   *ptr2;
	int			count = 0;
	int			len1,
				len2;

	ptr1 = GETARR(trg1);
	ptr2 = GETARR(trg2);

	len1 = ARRNELEM(trg1);
	len2 = ARRNELEM(trg2);

	/* explicit test is needed to avoid 0/0 division when both lengths are 0 */
	if (len1 <= 0 || len2 <= 0)
		return (float4) 0.0;

	while (ptr1 - GETARR(trg1) < len1 && ptr2 - GETARR(trg2) < len2)
	{
		int			res = CMPTRGM(ptr1, ptr2);

		if (res < 0)
			ptr1++;
		else if (res > 0)
			ptr2++;
		else
		{
			ptr1++;
			ptr2++;
			count++;
		}
	}

	/*
	 * If inexact then len2 is equal to count, because we don't know actual
	 * length of second string in inexact search and we can assume that count
	 * is a lower bound of len2.
	 */
	return CALCSML(count, len1, inexact ? count : len2);
}


/*
 * Returns whether trg2 contains all trigrams in trg1.
 * This relies on the trigram arrays being sorted.
 */
bool
trgm_contained_by(TRGM *trg1, TRGM *trg2)
{
	trgm	   *ptr1,
			   *ptr2;
	int			len1,
				len2;

	ptr1 = GETARR(trg1);
	ptr2 = GETARR(trg2);

	len1 = ARRNELEM(trg1);
	len2 = ARRNELEM(trg2);

	while (ptr1 - GETARR(trg1) < len1 && ptr2 - GETARR(trg2) < len2)
	{
		int			res = CMPTRGM(ptr1, ptr2);

		if (res < 0)
			return false;
		else if (res > 0)
			ptr2++;
		else
		{
			ptr1++;
			ptr2++;
		}
	}
	if (ptr1 - GETARR(trg1) < len1)
		return false;
	else
		return true;
}

/*
 * Return a palloc'd boolean array showing, for each trigram in "query",
 * whether it is present in the trigram array "key".
 * This relies on the "key" array being sorted, but "query" need not be.
 */
bool *
trgm_presence_map(TRGM *query, TRGM *key)
{
	bool	   *result;
	trgm	   *ptrq = GETARR(query),
			   *ptrk = GETARR(key);
	int			lenq = ARRNELEM(query),
				lenk = ARRNELEM(key),
				i;

	result = (bool *) palloc0(lenq * sizeof(bool));

	/* for each query trigram, do a binary search in the key array */
	for (i = 0; i < lenq; i++)
	{
		int			lo = 0;
		int			hi = lenk;

		while (lo < hi)
		{
			int			mid = (lo + hi) / 2;
			int			res = CMPTRGM(ptrq, ptrk + mid);

			if (res < 0)
				hi = mid;
			else if (res > 0)
				lo = mid + 1;
			else
			{
				result[i] = true;
				break;
			}
		}
		ptrq++;
	}

	return result;
}

Datum
similarity(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	TRGM	   *trg1,
			   *trg2;
	float4		res;

	trg1 = generate_trgm(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1));
	trg2 = generate_trgm(VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2));

	res = cnt_sml(trg1, trg2, false);

	pfree(trg1);
	pfree(trg2);
	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);

	PG_RETURN_FLOAT4(res);
}

Datum
word_similarity(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   0);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(res);
}

Datum
strict_word_similarity(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   WORD_SIMILARITY_STRICT);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(res);
}

Datum
similarity_dist(PG_FUNCTION_ARGS)
{
	float4		res = DatumGetFloat4(DirectFunctionCall2(similarity,
														 PG_GETARG_DATUM(0),
														 PG_GETARG_DATUM(1)));

	PG_RETURN_FLOAT4(1.0 - res);
}

Datum
similarity_op(PG_FUNCTION_ARGS)
{
	float4		res = DatumGetFloat4(DirectFunctionCall2(similarity,
														 PG_GETARG_DATUM(0),
														 PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(res >= similarity_threshold);
}

Datum
word_similarity_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   WORD_SIMILARITY_CHECK_ONLY);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_BOOL(res >= word_similarity_threshold);
}

Datum
word_similarity_commutator_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   WORD_SIMILARITY_CHECK_ONLY);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_BOOL(res >= word_similarity_threshold);
}

Datum
word_similarity_dist_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   0);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(1.0 - res);
}

Datum
word_similarity_dist_commutator_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   0);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(1.0 - res);
}

Datum
strict_word_similarity_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   WORD_SIMILARITY_CHECK_ONLY | WORD_SIMILARITY_STRICT);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_BOOL(res >= strict_word_similarity_threshold);
}

Datum
strict_word_similarity_commutator_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   WORD_SIMILARITY_CHECK_ONLY | WORD_SIMILARITY_STRICT);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_BOOL(res >= strict_word_similarity_threshold);
}

Datum
strict_word_similarity_dist_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   WORD_SIMILARITY_STRICT);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(1.0 - res);
}

Datum
strict_word_similarity_dist_commutator_op(PG_FUNCTION_ARGS)
{
	text	   *in1 = PG_GETARG_TEXT_PP(0);
	text	   *in2 = PG_GETARG_TEXT_PP(1);
	float4		res;

	res = calc_word_similarity(VARDATA_ANY(in2), VARSIZE_ANY_EXHDR(in2),
							   VARDATA_ANY(in1), VARSIZE_ANY_EXHDR(in1),
							   WORD_SIMILARITY_STRICT);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_FLOAT4(1.0 - res);
}
