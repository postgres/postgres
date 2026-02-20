/*
 * contrib/pg_trgm/trgm_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation_d.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "trgm.h"
#include "tsearch/ts_locale.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_trgm",
					.version = PG_VERSION
);

/* GUC variables */
double		similarity_threshold = 0.3f;
double		word_similarity_threshold = 0.6f;
double		strict_word_similarity_threshold = 0.5f;

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

static int	CMPTRGM_CHOOSE(const void *a, const void *b);
int			(*CMPTRGM) (const void *a, const void *b) = CMPTRGM_CHOOSE;

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
 * A growable array of trigrams
 *
 * The actual array of trigrams is in 'datum'.  Note that the other fields in
 * 'datum', i.e. datum->flags and the varlena length, are not kept up to date
 * when items are added to the growable array.  We merely reserve the space
 * for them here.  You must fill those other fields before using 'datum' as a
 * proper TRGM datum.
 */
typedef struct
{
	TRGM	   *datum;			/* trigram array */
	int			length;			/* number of trigrams in the array */
	int			allocated;		/* allocated size of 'datum' (# of trigrams) */
} growable_trgm_array;

/*
 * Allocate a new growable array.
 *
 * 'slen' is the size of the source string that we're extracting the trigrams
 * from.  It is used to choose the initial size of the array.
 */
static void
init_trgm_array(growable_trgm_array *arr, int slen)
{
	size_t		init_size;

	/*
	 * In the extreme case, the input string consists entirely of one
	 * character words, like "a b c", where each word is expanded to two
	 * trigrams.  This is not a strict upper bound though, because when
	 * IGNORECASE is defined, we convert the input string to lowercase before
	 * extracting the trigrams, which in rare cases can expand one input
	 * character into multiple characters.
	 */
	init_size = (size_t) slen + 1;

	/*
	 * Guard against possible overflow in the palloc request.  (We don't worry
	 * about the additive constants, since palloc can detect requests that are
	 * a little above MaxAllocSize --- we just need to prevent integer
	 * overflow in the multiplications.)
	 */
	if (init_size > MaxAllocSize / sizeof(trgm))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory")));

	arr->datum = palloc(CALCGTSIZE(ARRKEY, init_size));
	arr->allocated = init_size;
	arr->length = 0;
}

/* Make sure the array can hold at least 'needed' more trigrams */
static void
enlarge_trgm_array(growable_trgm_array *arr, int needed)
{
	size_t		new_needed = (size_t) arr->length + needed;

	if (new_needed > arr->allocated)
	{
		/* Guard against possible overflow, like in init_trgm_array */
		if (new_needed > MaxAllocSize / sizeof(trgm))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory")));

		arr->datum = repalloc(arr->datum, CALCGTSIZE(ARRKEY, new_needed));
		arr->allocated = new_needed;
	}
}

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
							 0.3f,
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
							 0.6f,
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
							 0.5f,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_trgm");
}

#define CMPCHAR(a,b) ( ((a)==(b)) ? 0 : ( ((a)<(b)) ? -1 : 1 ) )

/*
 * Functions for comparing two trgms while treating each char as "signed char" or
 * "unsigned char".
 */
static inline int
CMPTRGM_SIGNED(const void *a, const void *b)
{
#define CMPPCHAR_S(a,b,i)  CMPCHAR( *(((const signed char*)(a))+i), *(((const signed char*)(b))+i) )

	return CMPPCHAR_S(a, b, 0) ? CMPPCHAR_S(a, b, 0)
		: (CMPPCHAR_S(a, b, 1) ? CMPPCHAR_S(a, b, 1)
		   : CMPPCHAR_S(a, b, 2));
}

static inline int
CMPTRGM_UNSIGNED(const void *a, const void *b)
{
#define CMPPCHAR_UNS(a,b,i)  CMPCHAR( *(((const unsigned char*)(a))+i), *(((const unsigned char*)(b))+i) )

	return CMPPCHAR_UNS(a, b, 0) ? CMPPCHAR_UNS(a, b, 0)
		: (CMPPCHAR_UNS(a, b, 1) ? CMPPCHAR_UNS(a, b, 1)
		   : CMPPCHAR_UNS(a, b, 2));
}

/*
 * This gets called on the first call. It replaces the function pointer so
 * that subsequent calls are routed directly to the chosen implementation.
 */
static int
CMPTRGM_CHOOSE(const void *a, const void *b)
{
	if (GetDefaultCharSignedness())
		CMPTRGM = CMPTRGM_SIGNED;
	else
		CMPTRGM = CMPTRGM_UNSIGNED;

	return CMPTRGM(a, b);
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
find_word(char *str, int lenstr, char **endword)
{
	char	   *beginword = str;
	const char *endstr = str + lenstr;

	while (beginword < endstr)
	{
		int			clen = pg_mblen_range(beginword, endstr);

		if (ISWORDCHR(beginword, clen))
			break;
		beginword += clen;
	}

	if (beginword >= endstr)
		return NULL;

	*endword = beginword;
	while (*endword < endstr)
	{
		int			clen = pg_mblen_range(*endword, endstr);

		if (!ISWORDCHR(*endword, clen))
			break;
		*endword += clen;
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
 * Adds trigrams from the word in 'str' (already padded if necessary).
 */
static void
make_trigrams(growable_trgm_array *dst, char *str, int bytelen)
{
	trgm	   *tptr;
	char	   *ptr = str;

	if (bytelen < 3)
		return;

	/* max number of trigrams = strlen - 2 */
	enlarge_trgm_array(dst, bytelen - 2);
	tptr = GETARR(dst->datum) + dst->length;

	if (pg_encoding_max_length(GetDatabaseEncoding()) == 1)
	{
		while (ptr < str + bytelen - 2)
		{
			CPTRGM(tptr, ptr);
			ptr++;
			tptr++;
		}
	}
	else
	{
		int			lenfirst,
					lenmiddle,
					lenlast;
		char	   *endptr;

		/*
		 * Fast path as long as there are no multibyte characters
		 */
		if (!IS_HIGHBIT_SET(ptr[0]) && !IS_HIGHBIT_SET(ptr[1]))
		{
			while (!IS_HIGHBIT_SET(ptr[2]))
			{
				CPTRGM(tptr, ptr);
				ptr++;
				tptr++;

				if (ptr == str + bytelen - 2)
					goto done;
			}

			lenfirst = 1;
			lenmiddle = 1;
			lenlast = pg_mblen_unbounded(ptr + 2);
		}
		else
		{
			lenfirst = pg_mblen_unbounded(ptr);
			if (ptr + lenfirst >= str + bytelen)
				goto done;
			lenmiddle = pg_mblen_unbounded(ptr + lenfirst);
			if (ptr + lenfirst + lenmiddle >= str + bytelen)
				goto done;
			lenlast = pg_mblen_unbounded(ptr + lenfirst + lenmiddle);
		}

		/*
		 * Slow path to handle any remaining multibyte characters
		 *
		 * As we go, 'ptr' points to the beginning of the current
		 * three-character string and 'endptr' points to just past it.
		 */
		endptr = ptr + lenfirst + lenmiddle + lenlast;
		while (endptr <= str + bytelen)
		{
			compact_trigram(tptr, ptr, endptr - ptr);
			tptr++;

			/* Advance to the next character */
			if (endptr == str + bytelen)
				break;
			ptr += lenfirst;
			lenfirst = lenmiddle;
			lenmiddle = lenlast;
			lenlast = pg_mblen_unbounded(endptr);
			endptr += lenlast;
		}
	}

done:
	dst->length = tptr - GETARR(dst->datum);
	Assert(dst->length <= dst->allocated);
}

/*
 * Make array of trigrams without sorting and removing duplicate items.
 *
 * dst: where to return the array of trigrams.
 * str: source string, of length slen bytes.
 * bounds_p: where to return bounds of trigrams (if needed).
 */
static void
generate_trgm_only(growable_trgm_array *dst, char *str, int slen, TrgmBound **bounds_p)
{
	size_t		buflen;
	char	   *buf;
	int			bytelen;
	char	   *bword,
			   *eword;
	TrgmBound  *bounds = NULL;
	int			bounds_allocated = 0;

	init_trgm_array(dst, slen);

	/*
	 * If requested, allocate an array for the bounds, with the same size as
	 * the trigram array.
	 */
	if (bounds_p)
	{
		bounds_allocated = dst->allocated;
		bounds = *bounds_p = palloc0_array(TrgmBound, bounds_allocated);
	}

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
		return;

	/*
	 * Allocate a buffer for case-folded, blank-padded words.
	 *
	 * As an initial guess, allocate a buffer large enough to hold the
	 * original string with padding, which is always enough when compiled with
	 * !IGNORECASE.  If the case-folding produces a string longer than the
	 * original, we'll grow the buffer.
	 */
	buflen = (size_t) slen + 4;
	buf = (char *) palloc(buflen);
	if (LPADDING > 0)
	{
		*buf = ' ';
		if (LPADDING > 1)
			*(buf + 1) = ' ';
	}

	eword = str;
	while ((bword = find_word(eword, slen - (eword - str), &eword)) != NULL)
	{
		int			oldlen;

		/* Convert word to lower case before extracting trigrams from it */
#ifdef IGNORECASE
		{
			char	   *lowered;

			lowered = str_tolower(bword, eword - bword, DEFAULT_COLLATION_OID);
			bytelen = strlen(lowered);

			/* grow the buffer if necessary */
			if (bytelen > buflen - 4)
			{
				pfree(buf);
				buflen = (size_t) bytelen + 4;
				buf = (char *) palloc(buflen);
				if (LPADDING > 0)
				{
					*buf = ' ';
					if (LPADDING > 1)
						*(buf + 1) = ' ';
				}
			}
			memcpy(buf + LPADDING, lowered, bytelen);
			pfree(lowered);
		}
#else
		bytelen = eword - bword;
		memcpy(buf + LPADDING, bword, bytelen);
#endif

		buf[LPADDING + bytelen] = ' ';
		buf[LPADDING + bytelen + 1] = ' ';

		/* Calculate trigrams marking their bounds if needed */
		oldlen = dst->length;
		make_trigrams(dst, buf, bytelen + LPADDING + RPADDING);
		if (bounds)
		{
			if (bounds_allocated < dst->length)
			{
				bounds = *bounds_p = repalloc0_array(bounds, TrgmBound, bounds_allocated, dst->allocated);
				bounds_allocated = dst->allocated;
			}

			bounds[oldlen] |= TRGM_BOUND_LEFT;
			bounds[dst->length - 1] |= TRGM_BOUND_RIGHT;
		}
	}

	pfree(buf);
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
	growable_trgm_array arr;
	int			len;

	generate_trgm_only(&arr, str, slen, NULL);
	len = arr.length;
	trg = arr.datum;
	trg->flag = ARRKEY;

	/*
	 * Make trigrams unique.
	 */
	if (len > 1)
	{
		qsort(GETARR(trg), len, sizeof(trgm), comp_trgm);
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

	return pg_cmp_s32(p1->index, p2->index);
}

/*
 * Iterative search function which calculates maximum similarity with word in
 * the string. Maximum similarity is only calculated only if the flag
 * WORD_SIMILARITY_CHECK_ONLY isn't set.
 *
 * trg2indexes: array which stores indexes of the array "found".
 * found: array which stores true of false values.
 * ulen1: count of unique trigrams of array "trg1".
 * len2: length of array "trg2" and array "trg2indexes".
 * len: length of the array "found".
 * flags: set of boolean flags parameterizing similarity calculation.
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
		int			trgindex;

		CHECK_FOR_INTERRUPTS();

		/* Get index of next trigram */
		trgindex = trg2indexes[i];

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
	growable_trgm_array trg1;
	growable_trgm_array trg2;
	int			len1,
				len2,
				len,
				i,
				j,
				ulen1;
	int		   *trg2indexes;
	float4		result;
	TrgmBound  *bounds = NULL;

	/* Make positional trigrams */

	generate_trgm_only(&trg1, str1, slen1, NULL);
	len1 = trg1.length;
	generate_trgm_only(&trg2, str2, slen2, (flags & WORD_SIMILARITY_STRICT) ? &bounds : NULL);
	len2 = trg2.length;

	ptrg = make_positional_trgm(GETARR(trg1.datum), len1, GETARR(trg2.datum), len2);
	len = len1 + len2;
	qsort(ptrg, len, sizeof(pos_trgm), comp_ptrgm);

	pfree(trg1.datum);
	pfree(trg2.datum);

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
 *
 * Returns pointer to end+1 of the found substring in the source string.
 * Returns NULL if no word found (in which case buf, bytelen is not set)
 *
 * If the found word is bounded by non-word characters or string boundaries
 * then this function will include corresponding padding spaces into buf.
 */
static const char *
get_wildcard_part(const char *str, int lenstr,
				  char *buf, int *bytelen)
{
	const char *beginword = str;
	const char *endword;
	const char *endstr = str + lenstr;
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
	while (beginword < endstr)
	{
		clen = pg_mblen_range(beginword, endstr);

		if (in_escape)
		{
			if (ISWORDCHR(beginword, clen))
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
			else if (ISWORDCHR(beginword, clen))
				break;
			else
				in_leading_wildcard_meta = false;
		}
		beginword += clen;
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
	if (!in_leading_wildcard_meta)
	{
		if (LPADDING > 0)
		{
			*s++ = ' ';
			if (LPADDING > 1)
				*s++ = ' ';
		}
	}

	/*
	 * Copy data into buf until wildcard meta-character, non-word character or
	 * string boundary.  Strip escapes during copy.
	 */
	endword = beginword;
	while (endword < endstr)
	{
		clen = pg_mblen_range(endword, endstr);
		if (in_escape)
		{
			if (ISWORDCHR(endword, clen))
			{
				memcpy(s, endword, clen);
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
			else if (ISWORDCHR(endword, clen))
			{
				memcpy(s, endword, clen);
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
			if (RPADDING > 1)
				*s++ = ' ';
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
	growable_trgm_array arr;
	char	   *buf;
	int			len,
				bytelen;
	const char *eword;

	if (slen + LPADDING + RPADDING < 3 || slen == 0)
	{
		trg = (TRGM *) palloc(TRGMHDRSIZE);
		trg->flag = ARRKEY;
		SET_VARSIZE(trg, TRGMHDRSIZE);
		return trg;
	}

	init_trgm_array(&arr, slen);

	/* Allocate a buffer for blank-padded, but not yet case-folded, words */
	buf = palloc(sizeof(char) * (slen + 4));

	/*
	 * Extract trigrams from each substring extracted by get_wildcard_part.
	 */
	eword = str;
	while ((eword = get_wildcard_part(eword, slen - (eword - str),
									  buf, &bytelen)) != NULL)
	{
		char	   *word;

#ifdef IGNORECASE
		word = str_tolower(buf, bytelen, DEFAULT_COLLATION_OID);
		bytelen = strlen(word);
#else
		word = buf;
#endif

		/*
		 * count trigrams
		 */
		make_trigrams(&arr, word, bytelen);

#ifdef IGNORECASE
		pfree(word);
#endif
	}

	pfree(buf);

	/*
	 * Make trigrams unique.
	 */
	trg = arr.datum;
	len = arr.length;
	if (len > 1)
	{
		qsort(GETARR(trg), len, sizeof(trgm), comp_trgm);
		len = qunique(GETARR(trg), len, sizeof(trgm), comp_trgm);
	}

	trg->flag = ARRKEY;
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

	a = construct_array_builtin(d, ARRNELEM(trg), TEXTOID);

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
