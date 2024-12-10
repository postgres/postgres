/*-------------------------------------------------------------------------
 *
 * spell.c
 *		Normalizing word with ISpell
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * Ispell dictionary
 * -----------------
 *
 * Rules of dictionaries are defined in two files with .affix and .dict
 * extensions. They are used by spell checker programs Ispell and Hunspell.
 *
 * An .affix file declares morphological rules to get a basic form of words.
 * The format of an .affix file has different structure for Ispell and Hunspell
 * dictionaries. The Hunspell format is more complicated. But when an .affix
 * file is imported and compiled, it is stored in the same structure AffixNode.
 *
 * A .dict file stores a list of basic forms of words with references to
 * affix rules. The format of a .dict file has the same structure for Ispell
 * and Hunspell dictionaries.
 *
 * Compilation of a dictionary
 * ---------------------------
 *
 * A compiled dictionary is stored in the IspellDict structure. Compilation of
 * a dictionary is divided into the several steps:
 *	- NIImportDictionary() - stores each word of a .dict file in the
 *	  temporary Spell field.
 *	- NIImportAffixes() - stores affix rules of an .affix file in the
 *	  Affix field (not temporary) if an .affix file has the Ispell format.
 *	  -> NIImportOOAffixes() - stores affix rules if an .affix file has the
 *		 Hunspell format. The AffixData field is initialized if AF parameter
 *		 is defined.
 *	- NISortDictionary() - builds a prefix tree (Trie) from the words list
 *	  and stores it in the Dictionary field. The words list is got from the
 *	  Spell field. The AffixData field is initialized if AF parameter is not
 *	  defined.
 *	- NISortAffixes():
 *	  - builds a list of compound affixes from the affix list and stores it
 *		in the CompoundAffix.
 *	  - builds prefix trees (Trie) from the affix list for prefixes and suffixes
 *		and stores them in Suffix and Prefix fields.
 *	  The affix list is got from the Affix field.
 *
 * Memory management
 * -----------------
 *
 * The IspellDict structure has the Spell field which is used only in compile
 * time. The Spell field stores a words list. It can take a lot of memory.
 * Therefore when a dictionary is compiled this field is cleared by
 * NIFinishBuild().
 *
 * All resources which should cleared by NIFinishBuild() is initialized using
 * tmpalloc() and tmpalloc0().
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/spell.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "miscadmin.h"
#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"


/*
 * Initialization requires a lot of memory that's not needed
 * after the initialization is done.  During initialization,
 * CurrentMemoryContext is the long-lived memory context associated
 * with the dictionary cache entry.  We keep the short-lived stuff
 * in the Conf->buildCxt context.
 */
#define tmpalloc(sz)  MemoryContextAlloc(Conf->buildCxt, (sz))
#define tmpalloc0(sz)  MemoryContextAllocZero(Conf->buildCxt, (sz))

/*
 * Prepare for constructing an ISpell dictionary.
 *
 * The IspellDict struct is assumed to be zeroed when allocated.
 */
void
NIStartBuild(IspellDict *Conf)
{
	/*
	 * The temp context is a child of CurTransactionContext, so that it will
	 * go away automatically on error.
	 */
	Conf->buildCxt = AllocSetContextCreate(CurTransactionContext,
										   "Ispell dictionary init context",
										   ALLOCSET_DEFAULT_SIZES);
}

/*
 * Clean up when dictionary construction is complete.
 */
void
NIFinishBuild(IspellDict *Conf)
{
	/* Release no-longer-needed temp memory */
	MemoryContextDelete(Conf->buildCxt);
	/* Just for cleanliness, zero the now-dangling pointers */
	Conf->buildCxt = NULL;
	Conf->Spell = NULL;
	Conf->firstfree = NULL;
	Conf->CompoundAffixFlags = NULL;
}


/*
 * "Compact" palloc: allocate without extra palloc overhead.
 *
 * Since we have no need to free the ispell data items individually, there's
 * not much value in the per-chunk overhead normally consumed by palloc.
 * Getting rid of it is helpful since ispell can allocate a lot of small nodes.
 *
 * We currently pre-zero all data allocated this way, even though some of it
 * doesn't need that.  The cpalloc and cpalloc0 macros are just documentation
 * to indicate which allocations actually require zeroing.
 */
#define COMPACT_ALLOC_CHUNK 8192	/* amount to get from palloc at once */
#define COMPACT_MAX_REQ		1024	/* must be < COMPACT_ALLOC_CHUNK */

static void *
compact_palloc0(IspellDict *Conf, size_t size)
{
	void	   *result;

	/* Should only be called during init */
	Assert(Conf->buildCxt != NULL);

	/* No point in this for large chunks */
	if (size > COMPACT_MAX_REQ)
		return palloc0(size);

	/* Keep everything maxaligned */
	size = MAXALIGN(size);

	/* Need more space? */
	if (size > Conf->avail)
	{
		Conf->firstfree = palloc0(COMPACT_ALLOC_CHUNK);
		Conf->avail = COMPACT_ALLOC_CHUNK;
	}

	result = Conf->firstfree;
	Conf->firstfree += size;
	Conf->avail -= size;

	return result;
}

#define cpalloc(size) compact_palloc0(Conf, size)
#define cpalloc0(size) compact_palloc0(Conf, size)

static char *
cpstrdup(IspellDict *Conf, const char *str)
{
	char	   *res = cpalloc(strlen(str) + 1);

	strcpy(res, str);
	return res;
}


/*
 * Apply lowerstr(), producing a temporary result (in the buildCxt).
 */
static char *
lowerstr_ctx(IspellDict *Conf, const char *src)
{
	MemoryContext saveCtx;
	char	   *dst;

	saveCtx = MemoryContextSwitchTo(Conf->buildCxt);
	dst = lowerstr(src);
	MemoryContextSwitchTo(saveCtx);

	return dst;
}

#define MAX_NORM 1024
#define MAXNORMLEN 256

#define STRNCMP(s,p)	strncmp( (s), (p), strlen(p) )
#define GETWCHAR(W,L,N,T) ( ((const uint8*)(W))[ ((T)==FF_PREFIX) ? (N) : ( (L) - 1 - (N) ) ] )
#define GETCHAR(A,N,T)	  GETWCHAR( (A)->repl, (A)->replen, N, T )

static const char *VoidString = "";

static int
cmpspell(const void *s1, const void *s2)
{
	return strcmp((*(SPELL *const *) s1)->word, (*(SPELL *const *) s2)->word);
}

static int
cmpspellaffix(const void *s1, const void *s2)
{
	return strcmp((*(SPELL *const *) s1)->p.flag,
				  (*(SPELL *const *) s2)->p.flag);
}

static int
cmpcmdflag(const void *f1, const void *f2)
{
	CompoundAffixFlag *fv1 = (CompoundAffixFlag *) f1,
			   *fv2 = (CompoundAffixFlag *) f2;

	Assert(fv1->flagMode == fv2->flagMode);

	if (fv1->flagMode == FM_NUM)
	{
		if (fv1->flag.i == fv2->flag.i)
			return 0;

		return (fv1->flag.i > fv2->flag.i) ? 1 : -1;
	}

	return strcmp(fv1->flag.s, fv2->flag.s);
}

static char *
findchar(char *str, int c)
{
	while (*str)
	{
		if (t_iseq(str, c))
			return str;
		str += pg_mblen(str);
	}

	return NULL;
}

static char *
findchar2(char *str, int c1, int c2)
{
	while (*str)
	{
		if (t_iseq(str, c1) || t_iseq(str, c2))
			return str;
		str += pg_mblen(str);
	}

	return NULL;
}


/* backward string compare for suffix tree operations */
static int
strbcmp(const unsigned char *s1, const unsigned char *s2)
{
	int			l1 = strlen((const char *) s1) - 1,
				l2 = strlen((const char *) s2) - 1;

	while (l1 >= 0 && l2 >= 0)
	{
		if (s1[l1] < s2[l2])
			return -1;
		if (s1[l1] > s2[l2])
			return 1;
		l1--;
		l2--;
	}
	if (l1 < l2)
		return -1;
	if (l1 > l2)
		return 1;

	return 0;
}

static int
strbncmp(const unsigned char *s1, const unsigned char *s2, size_t count)
{
	int			l1 = strlen((const char *) s1) - 1,
				l2 = strlen((const char *) s2) - 1,
				l = count;

	while (l1 >= 0 && l2 >= 0 && l > 0)
	{
		if (s1[l1] < s2[l2])
			return -1;
		if (s1[l1] > s2[l2])
			return 1;
		l1--;
		l2--;
		l--;
	}
	if (l == 0)
		return 0;
	if (l1 < l2)
		return -1;
	if (l1 > l2)
		return 1;
	return 0;
}

/*
 * Compares affixes.
 * First compares the type of an affix. Prefixes should go before affixes.
 * If types are equal then compares replaceable string.
 */
static int
cmpaffix(const void *s1, const void *s2)
{
	const AFFIX *a1 = (const AFFIX *) s1;
	const AFFIX *a2 = (const AFFIX *) s2;

	if (a1->type < a2->type)
		return -1;
	if (a1->type > a2->type)
		return 1;
	if (a1->type == FF_PREFIX)
		return strcmp(a1->repl, a2->repl);
	else
		return strbcmp((const unsigned char *) a1->repl,
					   (const unsigned char *) a2->repl);
}

/*
 * Gets an affix flag from the set of affix flags (sflagset).
 *
 * Several flags can be stored in a single string. Flags can be represented by:
 * - 1 character (FM_CHAR). A character may be Unicode.
 * - 2 characters (FM_LONG). A character may be Unicode.
 * - numbers from 1 to 65000 (FM_NUM).
 *
 * Depending on the flagMode an affix string can have the following format:
 * - FM_CHAR: ABCD
 *	 Here we have 4 flags: A, B, C and D
 * - FM_LONG: ABCDE*
 *	 Here we have 3 flags: AB, CD and E*
 * - FM_NUM: 200,205,50
 *	 Here we have 3 flags: 200, 205 and 50
 *
 * Conf: current dictionary.
 * sflagset: the set of affix flags. Returns a reference to the start of a next
 *			 affix flag.
 * sflag: returns an affix flag from sflagset.
 */
static void
getNextFlagFromString(IspellDict *Conf, const char **sflagset, char *sflag)
{
	int32		s;
	char	   *next;
	const char *sbuf = *sflagset;
	int			maxstep;
	bool		stop = false;
	bool		met_comma = false;

	maxstep = (Conf->flagMode == FM_LONG) ? 2 : 1;

	while (**sflagset)
	{
		switch (Conf->flagMode)
		{
			case FM_LONG:
			case FM_CHAR:
				COPYCHAR(sflag, *sflagset);
				sflag += pg_mblen(*sflagset);

				/* Go to start of the next flag */
				*sflagset += pg_mblen(*sflagset);

				/* Check if we get all characters of flag */
				maxstep--;
				stop = (maxstep == 0);
				break;
			case FM_NUM:
				s = strtol(*sflagset, &next, 10);
				if (*sflagset == next || errno == ERANGE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix flag \"%s\"", *sflagset)));
				if (s < 0 || s > FLAGNUM_MAXSIZE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("affix flag \"%s\" is out of range",
									*sflagset)));
				sflag += sprintf(sflag, "%0d", s);

				/* Go to start of the next flag */
				*sflagset = next;
				while (**sflagset)
				{
					if (t_isdigit(*sflagset))
					{
						if (!met_comma)
							ereport(ERROR,
									(errcode(ERRCODE_CONFIG_FILE_ERROR),
									 errmsg("invalid affix flag \"%s\"",
											*sflagset)));
						break;
					}
					else if (t_iseq(*sflagset, ','))
					{
						if (met_comma)
							ereport(ERROR,
									(errcode(ERRCODE_CONFIG_FILE_ERROR),
									 errmsg("invalid affix flag \"%s\"",
											*sflagset)));
						met_comma = true;
					}
					else if (!t_isspace(*sflagset))
					{
						ereport(ERROR,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("invalid character in affix flag \"%s\"",
										*sflagset)));
					}

					*sflagset += pg_mblen(*sflagset);
				}
				stop = true;
				break;
			default:
				elog(ERROR, "unrecognized type of Conf->flagMode: %d",
					 Conf->flagMode);
		}

		if (stop)
			break;
	}

	if (Conf->flagMode == FM_LONG && maxstep > 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid affix flag \"%s\" with \"long\" flag value",
						sbuf)));

	*sflag = '\0';
}

/*
 * Checks if the affix set Conf->AffixData[affix] contains affixflag.
 * Conf->AffixData[affix] does not contain affixflag if this flag is not used
 * actually by the .dict file.
 *
 * Conf: current dictionary.
 * affix: index of the Conf->AffixData array.
 * affixflag: the affix flag.
 *
 * Returns true if the string Conf->AffixData[affix] contains affixflag,
 * otherwise returns false.
 */
static bool
IsAffixFlagInUse(IspellDict *Conf, int affix, const char *affixflag)
{
	const char *flagcur;
	char		flag[BUFSIZ];

	if (*affixflag == 0)
		return true;

	Assert(affix < Conf->nAffixData);

	flagcur = Conf->AffixData[affix];

	while (*flagcur)
	{
		getNextFlagFromString(Conf, &flagcur, flag);
		/* Compare first affix flag in flagcur with affixflag */
		if (strcmp(flag, affixflag) == 0)
			return true;
	}

	/* Could not find affixflag */
	return false;
}

/*
 * Adds the new word into the temporary array Spell.
 *
 * Conf: current dictionary.
 * word: new word.
 * flag: set of affix flags. Single flag can be get by getNextFlagFromString().
 */
static void
NIAddSpell(IspellDict *Conf, const char *word, const char *flag)
{
	if (Conf->nspell >= Conf->mspell)
	{
		if (Conf->mspell)
		{
			Conf->mspell *= 2;
			Conf->Spell = (SPELL **) repalloc(Conf->Spell, Conf->mspell * sizeof(SPELL *));
		}
		else
		{
			Conf->mspell = 1024 * 20;
			Conf->Spell = (SPELL **) tmpalloc(Conf->mspell * sizeof(SPELL *));
		}
	}
	Conf->Spell[Conf->nspell] = (SPELL *) tmpalloc(SPELLHDRSZ + strlen(word) + 1);
	strcpy(Conf->Spell[Conf->nspell]->word, word);
	Conf->Spell[Conf->nspell]->p.flag = (*flag != '\0')
		? cpstrdup(Conf, flag) : VoidString;
	Conf->nspell++;
}

/*
 * Imports dictionary into the temporary array Spell.
 *
 * Note caller must already have applied get_tsearch_config_filename.
 *
 * Conf: current dictionary.
 * filename: path to the .dict file.
 */
void
NIImportDictionary(IspellDict *Conf, const char *filename)
{
	tsearch_readline_state trst;
	char	   *line;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open dictionary file \"%s\": %m",
						filename)));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		char	   *s,
				   *pstr;

		/* Set of affix flags */
		const char *flag;

		/* Extract flag from the line */
		flag = NULL;
		if ((s = findchar(line, '/')))
		{
			*s++ = '\0';
			flag = s;
			while (*s)
			{
				/* we allow only single encoded flags for faster works */
				if (pg_mblen(s) == 1 && t_isprint(s) && !t_isspace(s))
					s++;
				else
				{
					*s = '\0';
					break;
				}
			}
		}
		else
			flag = "";

		/* Remove trailing spaces */
		s = line;
		while (*s)
		{
			if (t_isspace(s))
			{
				*s = '\0';
				break;
			}
			s += pg_mblen(s);
		}
		pstr = lowerstr_ctx(Conf, line);

		NIAddSpell(Conf, pstr, flag);
		pfree(pstr);

		pfree(line);
	}
	tsearch_readline_end(&trst);
}

/*
 * Searches a basic form of word in the prefix tree. This word was generated
 * using an affix rule. This rule may not be presented in an affix set of
 * a basic form of word.
 *
 * For example, we have the entry in the .dict file:
 * meter/GMD
 *
 * The affix rule with the flag S:
 * SFX S   y	 ies		[^aeiou]y
 * is not presented here.
 *
 * The affix rule with the flag M:
 * SFX M   0	 's         .
 * is presented here.
 *
 * Conf: current dictionary.
 * word: basic form of word.
 * affixflag: affix flag, by which a basic form of word was generated.
 * flag: compound flag used to compare with StopMiddle->compoundflag.
 *
 * Returns 1 if the word was found in the prefix tree, else returns 0.
 */
static int
FindWord(IspellDict *Conf, const char *word, const char *affixflag, int flag)
{
	SPNode	   *node = Conf->Dictionary;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	const uint8 *ptr = (const uint8 *) word;

	flag &= FF_COMPOUNDFLAGMASK;

	while (node && *ptr)
	{
		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (StopMiddle->val == *ptr)
			{
				if (*(ptr + 1) == '\0' && StopMiddle->isword)
				{
					if (flag == 0)
					{
						/*
						 * The word can be formed only with another word. And
						 * in the flag parameter there is not a sign that we
						 * search compound words.
						 */
						if (StopMiddle->compoundflag & FF_COMPOUNDONLY)
							return 0;
					}
					else if ((flag & StopMiddle->compoundflag) == 0)
						return 0;

					/*
					 * Check if this affix rule is presented in the affix set
					 * with index StopMiddle->affix.
					 */
					if (IsAffixFlagInUse(Conf, StopMiddle->affix, affixflag))
						return 1;
				}
				node = StopMiddle->node;
				ptr++;
				break;
			}
			else if (StopMiddle->val < *ptr)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
		if (StopLow >= StopHigh)
			break;
	}
	return 0;
}

/*
 * Adds a new affix rule to the Affix field.
 *
 * Conf: current dictionary.
 * flag: affix flag ('\' in the below example).
 * flagflags: set of flags from the flagval field for this affix rule. This set
 *			  is listed after '/' character in the added string (repl).
 *
 *			  For example L flag in the hunspell_sample.affix:
 *			  SFX \   0 Y/L [^Y]
 *
 * mask: condition for search ('[^Y]' in the above example).
 * find: stripping characters from beginning (at prefix) or end (at suffix)
 *		 of the word ('0' in the above example, 0 means that there is not
 *		 stripping character).
 * repl: adding string after stripping ('Y' in the above example).
 * type: FF_SUFFIX or FF_PREFIX.
 */
static void
NIAddAffix(IspellDict *Conf, const char *flag, char flagflags, const char *mask,
		   const char *find, const char *repl, int type)
{
	AFFIX	   *Affix;

	if (Conf->naffixes >= Conf->maffixes)
	{
		if (Conf->maffixes)
		{
			Conf->maffixes *= 2;
			Conf->Affix = (AFFIX *) repalloc(Conf->Affix, Conf->maffixes * sizeof(AFFIX));
		}
		else
		{
			Conf->maffixes = 16;
			Conf->Affix = (AFFIX *) palloc(Conf->maffixes * sizeof(AFFIX));
		}
	}

	Affix = Conf->Affix + Conf->naffixes;

	/* This affix rule can be applied for words with any ending */
	if (strcmp(mask, ".") == 0 || *mask == '\0')
	{
		Affix->issimple = 1;
		Affix->isregis = 0;
	}
	/* This affix rule will use regis to search word ending */
	else if (RS_isRegis(mask))
	{
		Affix->issimple = 0;
		Affix->isregis = 1;
		RS_compile(&(Affix->reg.regis), (type == FF_SUFFIX),
				   *mask ? mask : VoidString);
	}
	/* This affix rule will use regex_t to search word ending */
	else
	{
		int			masklen;
		int			wmasklen;
		int			err;
		pg_wchar   *wmask;
		char	   *tmask;

		Affix->issimple = 0;
		Affix->isregis = 0;
		tmask = (char *) tmpalloc(strlen(mask) + 3);
		if (type == FF_SUFFIX)
			sprintf(tmask, "%s$", mask);
		else
			sprintf(tmask, "^%s", mask);

		masklen = strlen(tmask);
		wmask = (pg_wchar *) tmpalloc((masklen + 1) * sizeof(pg_wchar));
		wmasklen = pg_mb2wchar_with_len(tmask, wmask, masklen);

		/*
		 * The regex and all internal state created by pg_regcomp are
		 * allocated in the dictionary's memory context, and will be freed
		 * automatically when it is destroyed.
		 */
		Affix->reg.pregex = palloc(sizeof(regex_t));
		err = pg_regcomp(Affix->reg.pregex, wmask, wmasklen,
						 REG_ADVANCED | REG_NOSUB,
						 DEFAULT_COLLATION_OID);
		if (err)
		{
			char		errstr[100];

			pg_regerror(err, Affix->reg.pregex, errstr, sizeof(errstr));
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("invalid regular expression: %s", errstr)));
		}
	}

	Affix->flagflags = flagflags;
	if ((Affix->flagflags & FF_COMPOUNDONLY) || (Affix->flagflags & FF_COMPOUNDPERMITFLAG))
	{
		if ((Affix->flagflags & FF_COMPOUNDFLAG) == 0)
			Affix->flagflags |= FF_COMPOUNDFLAG;
	}
	Affix->flag = cpstrdup(Conf, flag);
	Affix->type = type;

	Affix->find = (find && *find) ? cpstrdup(Conf, find) : VoidString;
	if ((Affix->replen = strlen(repl)) > 0)
		Affix->repl = cpstrdup(Conf, repl);
	else
		Affix->repl = VoidString;
	Conf->naffixes++;
}

/* Parsing states for parse_affentry() and friends */
#define PAE_WAIT_MASK	0
#define PAE_INMASK		1
#define PAE_WAIT_FIND	2
#define PAE_INFIND		3
#define PAE_WAIT_REPL	4
#define PAE_INREPL		5
#define PAE_WAIT_TYPE	6
#define PAE_WAIT_FLAG	7

/*
 * Parse next space-separated field of an .affix file line.
 *
 * *str is the input pointer (will be advanced past field)
 * next is where to copy the field value to, with null termination
 *
 * The buffer at "next" must be of size BUFSIZ; we truncate the input to fit.
 *
 * Returns true if we found a field, false if not.
 */
static bool
get_nextfield(char **str, char *next)
{
	int			state = PAE_WAIT_MASK;
	int			avail = BUFSIZ;

	while (**str)
	{
		if (state == PAE_WAIT_MASK)
		{
			if (t_iseq(*str, '#'))
				return false;
			else if (!t_isspace(*str))
			{
				int			clen = pg_mblen(*str);

				if (clen < avail)
				{
					COPYCHAR(next, *str);
					next += clen;
					avail -= clen;
				}
				state = PAE_INMASK;
			}
		}
		else					/* state == PAE_INMASK */
		{
			if (t_isspace(*str))
			{
				*next = '\0';
				return true;
			}
			else
			{
				int			clen = pg_mblen(*str);

				if (clen < avail)
				{
					COPYCHAR(next, *str);
					next += clen;
					avail -= clen;
				}
			}
		}
		*str += pg_mblen(*str);
	}

	*next = '\0';

	return (state == PAE_INMASK);	/* OK if we got a nonempty field */
}

/*
 * Parses entry of an .affix file of MySpell or Hunspell format.
 *
 * An .affix file entry has the following format:
 * - header
 *	 <type>  <flag>  <cross_flag>  <flag_count>
 * - fields after header:
 *	 <type>  <flag>  <find>  <replace>	<mask>
 *
 * str is the input line
 * field values are returned to type etc, which must be buffers of size BUFSIZ.
 *
 * Returns number of fields found; any omitted fields are set to empty strings.
 */
static int
parse_ooaffentry(char *str, char *type, char *flag, char *find,
				 char *repl, char *mask)
{
	int			state = PAE_WAIT_TYPE;
	int			fields_read = 0;
	bool		valid = false;

	*type = *flag = *find = *repl = *mask = '\0';

	while (*str)
	{
		switch (state)
		{
			case PAE_WAIT_TYPE:
				valid = get_nextfield(&str, type);
				state = PAE_WAIT_FLAG;
				break;
			case PAE_WAIT_FLAG:
				valid = get_nextfield(&str, flag);
				state = PAE_WAIT_FIND;
				break;
			case PAE_WAIT_FIND:
				valid = get_nextfield(&str, find);
				state = PAE_WAIT_REPL;
				break;
			case PAE_WAIT_REPL:
				valid = get_nextfield(&str, repl);
				state = PAE_WAIT_MASK;
				break;
			case PAE_WAIT_MASK:
				valid = get_nextfield(&str, mask);
				state = -1;		/* force loop exit */
				break;
			default:
				elog(ERROR, "unrecognized state in parse_ooaffentry: %d",
					 state);
				break;
		}
		if (valid)
			fields_read++;
		else
			break;				/* early EOL */
		if (state < 0)
			break;				/* got all fields */
	}

	return fields_read;
}

/*
 * Parses entry of an .affix file of Ispell format
 *
 * An .affix file entry has the following format:
 * <mask>  >  [-<find>,]<replace>
 */
static bool
parse_affentry(char *str, char *mask, char *find, char *repl)
{
	int			state = PAE_WAIT_MASK;
	char	   *pmask = mask,
			   *pfind = find,
			   *prepl = repl;

	*mask = *find = *repl = '\0';

	while (*str)
	{
		if (state == PAE_WAIT_MASK)
		{
			if (t_iseq(str, '#'))
				return false;
			else if (!t_isspace(str))
			{
				COPYCHAR(pmask, str);
				pmask += pg_mblen(str);
				state = PAE_INMASK;
			}
		}
		else if (state == PAE_INMASK)
		{
			if (t_iseq(str, '>'))
			{
				*pmask = '\0';
				state = PAE_WAIT_FIND;
			}
			else if (!t_isspace(str))
			{
				COPYCHAR(pmask, str);
				pmask += pg_mblen(str);
			}
		}
		else if (state == PAE_WAIT_FIND)
		{
			if (t_iseq(str, '-'))
			{
				state = PAE_INFIND;
			}
			else if (t_isalpha(str) || t_iseq(str, '\'') /* english 's */ )
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
				state = PAE_INREPL;
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_INFIND)
		{
			if (t_iseq(str, ','))
			{
				*pfind = '\0';
				state = PAE_WAIT_REPL;
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(pfind, str);
				pfind += pg_mblen(str);
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_WAIT_REPL)
		{
			if (t_iseq(str, '-'))
			{
				break;			/* void repl */
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
				state = PAE_INREPL;
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_INREPL)
		{
			if (t_iseq(str, '#'))
			{
				*prepl = '\0';
				break;
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else
			elog(ERROR, "unrecognized state in parse_affentry: %d", state);

		str += pg_mblen(str);
	}

	*pmask = *pfind = *prepl = '\0';

	return (*mask && (*find || *repl));
}

/*
 * Sets a Hunspell options depending on flag type.
 */
static void
setCompoundAffixFlagValue(IspellDict *Conf, CompoundAffixFlag *entry,
						  char *s, uint32 val)
{
	if (Conf->flagMode == FM_NUM)
	{
		char	   *next;
		int			i;

		i = strtol(s, &next, 10);
		if (s == next || errno == ERANGE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid affix flag \"%s\"", s)));
		if (i < 0 || i > FLAGNUM_MAXSIZE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("affix flag \"%s\" is out of range", s)));

		entry->flag.i = i;
	}
	else
		entry->flag.s = cpstrdup(Conf, s);

	entry->flagMode = Conf->flagMode;
	entry->value = val;
}

/*
 * Sets up a correspondence for the affix parameter with the affix flag.
 *
 * Conf: current dictionary.
 * s: affix flag in string.
 * val: affix parameter.
 */
static void
addCompoundAffixFlagValue(IspellDict *Conf, char *s, uint32 val)
{
	CompoundAffixFlag *newValue;
	char		sbuf[BUFSIZ];
	char	   *sflag;
	int			clen;

	while (*s && t_isspace(s))
		s += pg_mblen(s);

	if (!*s)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("syntax error")));

	/* Get flag without \n */
	sflag = sbuf;
	while (*s && !t_isspace(s) && *s != '\n')
	{
		clen = pg_mblen(s);
		COPYCHAR(sflag, s);
		sflag += clen;
		s += clen;
	}
	*sflag = '\0';

	/* Resize array or allocate memory for array CompoundAffixFlag */
	if (Conf->nCompoundAffixFlag >= Conf->mCompoundAffixFlag)
	{
		if (Conf->mCompoundAffixFlag)
		{
			Conf->mCompoundAffixFlag *= 2;
			Conf->CompoundAffixFlags = (CompoundAffixFlag *)
				repalloc(Conf->CompoundAffixFlags,
						 Conf->mCompoundAffixFlag * sizeof(CompoundAffixFlag));
		}
		else
		{
			Conf->mCompoundAffixFlag = 10;
			Conf->CompoundAffixFlags = (CompoundAffixFlag *)
				tmpalloc(Conf->mCompoundAffixFlag * sizeof(CompoundAffixFlag));
		}
	}

	newValue = Conf->CompoundAffixFlags + Conf->nCompoundAffixFlag;

	setCompoundAffixFlagValue(Conf, newValue, sbuf, val);

	Conf->usecompound = true;
	Conf->nCompoundAffixFlag++;
}

/*
 * Returns a set of affix parameters which correspondence to the set of affix
 * flags s.
 */
static int
getCompoundAffixFlagValue(IspellDict *Conf, const char *s)
{
	uint32		flag = 0;
	CompoundAffixFlag *found,
				key;
	char		sflag[BUFSIZ];
	const char *flagcur;

	if (Conf->nCompoundAffixFlag == 0)
		return 0;

	flagcur = s;
	while (*flagcur)
	{
		getNextFlagFromString(Conf, &flagcur, sflag);
		setCompoundAffixFlagValue(Conf, &key, sflag, 0);

		found = (CompoundAffixFlag *)
			bsearch(&key, Conf->CompoundAffixFlags,
					Conf->nCompoundAffixFlag, sizeof(CompoundAffixFlag),
					cmpcmdflag);
		if (found != NULL)
			flag |= found->value;
	}

	return flag;
}

/*
 * Returns a flag set using the s parameter.
 *
 * If Conf->useFlagAliases is true then the s parameter is index of the
 * Conf->AffixData array and function returns its entry.
 * Else function returns the s parameter.
 */
static const char *
getAffixFlagSet(IspellDict *Conf, char *s)
{
	if (Conf->useFlagAliases && *s != '\0')
	{
		int			curaffix;
		char	   *end;

		curaffix = strtol(s, &end, 10);
		if (s == end || errno == ERANGE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid affix alias \"%s\"", s)));

		if (curaffix > 0 && curaffix < Conf->nAffixData)

			/*
			 * Do not subtract 1 from curaffix because empty string was added
			 * in NIImportOOAffixes
			 */
			return Conf->AffixData[curaffix];
		else if (curaffix > Conf->nAffixData)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid affix alias \"%s\"", s)));
		return VoidString;
	}
	else
		return s;
}

/*
 * Import an affix file that follows MySpell or Hunspell format.
 *
 * Conf: current dictionary.
 * filename: path to the .affix file.
 */
static void
NIImportOOAffixes(IspellDict *Conf, const char *filename)
{
	char		type[BUFSIZ],
			   *ptype = NULL;
	char		sflag[BUFSIZ];
	char		mask[BUFSIZ],
			   *pmask;
	char		find[BUFSIZ],
			   *pfind;
	char		repl[BUFSIZ],
			   *prepl;
	bool		isSuffix = false;
	int			naffix = 0,
				curaffix = 0;
	int			sflaglen = 0;
	char		flagflags = 0;
	tsearch_readline_state trst;
	char	   *recoded;

	/* read file to find any flag */
	Conf->usecompound = false;
	Conf->useFlagAliases = false;
	Conf->flagMode = FM_CHAR;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		if (*recoded == '\0' || t_isspace(recoded) || t_iseq(recoded, '#'))
		{
			pfree(recoded);
			continue;
		}

		if (STRNCMP(recoded, "COMPOUNDFLAG") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("COMPOUNDFLAG"),
									  FF_COMPOUNDFLAG);
		else if (STRNCMP(recoded, "COMPOUNDBEGIN") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("COMPOUNDBEGIN"),
									  FF_COMPOUNDBEGIN);
		else if (STRNCMP(recoded, "COMPOUNDLAST") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("COMPOUNDLAST"),
									  FF_COMPOUNDLAST);
		/* COMPOUNDLAST and COMPOUNDEND are synonyms */
		else if (STRNCMP(recoded, "COMPOUNDEND") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("COMPOUNDEND"),
									  FF_COMPOUNDLAST);
		else if (STRNCMP(recoded, "COMPOUNDMIDDLE") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("COMPOUNDMIDDLE"),
									  FF_COMPOUNDMIDDLE);
		else if (STRNCMP(recoded, "ONLYINCOMPOUND") == 0)
			addCompoundAffixFlagValue(Conf, recoded + strlen("ONLYINCOMPOUND"),
									  FF_COMPOUNDONLY);
		else if (STRNCMP(recoded, "COMPOUNDPERMITFLAG") == 0)
			addCompoundAffixFlagValue(Conf,
									  recoded + strlen("COMPOUNDPERMITFLAG"),
									  FF_COMPOUNDPERMITFLAG);
		else if (STRNCMP(recoded, "COMPOUNDFORBIDFLAG") == 0)
			addCompoundAffixFlagValue(Conf,
									  recoded + strlen("COMPOUNDFORBIDFLAG"),
									  FF_COMPOUNDFORBIDFLAG);
		else if (STRNCMP(recoded, "FLAG") == 0)
		{
			char	   *s = recoded + strlen("FLAG");

			while (*s && t_isspace(s))
				s += pg_mblen(s);

			if (*s)
			{
				if (STRNCMP(s, "long") == 0)
					Conf->flagMode = FM_LONG;
				else if (STRNCMP(s, "num") == 0)
					Conf->flagMode = FM_NUM;
				else if (STRNCMP(s, "default") != 0)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("Ispell dictionary supports only "
									"\"default\", \"long\", "
									"and \"num\" flag values")));
			}
		}

		pfree(recoded);
	}
	tsearch_readline_end(&trst);

	if (Conf->nCompoundAffixFlag > 1)
		qsort(Conf->CompoundAffixFlags, Conf->nCompoundAffixFlag,
			  sizeof(CompoundAffixFlag), cmpcmdflag);

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		int			fields_read;

		if (*recoded == '\0' || t_isspace(recoded) || t_iseq(recoded, '#'))
			goto nextline;

		fields_read = parse_ooaffentry(recoded, type, sflag, find, repl, mask);

		if (ptype)
			pfree(ptype);
		ptype = lowerstr_ctx(Conf, type);

		/* First try to parse AF parameter (alias compression) */
		if (STRNCMP(ptype, "af") == 0)
		{
			/* First line is the number of aliases */
			if (!Conf->useFlagAliases)
			{
				Conf->useFlagAliases = true;
				naffix = atoi(sflag);
				if (naffix <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid number of flag vector aliases")));

				/* Also reserve place for empty flag set */
				naffix++;

				Conf->AffixData = (const char **) palloc0(naffix * sizeof(char *));
				Conf->lenAffixData = Conf->nAffixData = naffix;

				/* Add empty flag set into AffixData */
				Conf->AffixData[curaffix] = VoidString;
				curaffix++;
			}
			/* Other lines are aliases */
			else
			{
				if (curaffix < naffix)
				{
					Conf->AffixData[curaffix] = cpstrdup(Conf, sflag);
					curaffix++;
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("number of aliases exceeds specified number %d",
									naffix - 1)));
			}
			goto nextline;
		}
		/* Else try to parse prefixes and suffixes */
		if (fields_read < 4 ||
			(STRNCMP(ptype, "sfx") != 0 && STRNCMP(ptype, "pfx") != 0))
			goto nextline;

		sflaglen = strlen(sflag);
		if (sflaglen == 0
			|| (sflaglen > 1 && Conf->flagMode == FM_CHAR)
			|| (sflaglen > 2 && Conf->flagMode == FM_LONG))
			goto nextline;

		/*--------
		 * Affix header. For example:
		 * SFX \ N 1
		 *--------
		 */
		if (fields_read == 4)
		{
			isSuffix = (STRNCMP(ptype, "sfx") == 0);
			if (t_iseq(find, 'y') || t_iseq(find, 'Y'))
				flagflags = FF_CROSSPRODUCT;
			else
				flagflags = 0;
		}
		/*--------
		 * Affix fields. For example:
		 * SFX \   0	Y/L [^Y]
		 *--------
		 */
		else
		{
			char	   *ptr;
			int			aflg = 0;

			/* Get flags after '/' (flags are case sensitive) */
			if ((ptr = strchr(repl, '/')) != NULL)
				aflg |= getCompoundAffixFlagValue(Conf,
												  getAffixFlagSet(Conf,
																  ptr + 1));
			/* Get lowercased version of string before '/' */
			prepl = lowerstr_ctx(Conf, repl);
			if ((ptr = strchr(prepl, '/')) != NULL)
				*ptr = '\0';
			pfind = lowerstr_ctx(Conf, find);
			pmask = lowerstr_ctx(Conf, mask);
			if (t_iseq(find, '0'))
				*pfind = '\0';
			if (t_iseq(repl, '0'))
				*prepl = '\0';

			NIAddAffix(Conf, sflag, flagflags | aflg, pmask, pfind, prepl,
					   isSuffix ? FF_SUFFIX : FF_PREFIX);
			pfree(prepl);
			pfree(pfind);
			pfree(pmask);
		}

nextline:
		pfree(recoded);
	}

	tsearch_readline_end(&trst);
	if (ptype)
		pfree(ptype);
}

/*
 * import affixes
 *
 * Note caller must already have applied get_tsearch_config_filename
 *
 * This function is responsible for parsing ispell ("old format") affix files.
 * If we realize that the file contains new-format commands, we pass off the
 * work to NIImportOOAffixes(), which will re-read the whole file.
 */
void
NIImportAffixes(IspellDict *Conf, const char *filename)
{
	char	   *pstr = NULL;
	char		flag[BUFSIZ];
	char		mask[BUFSIZ];
	char		find[BUFSIZ];
	char		repl[BUFSIZ];
	char	   *s;
	bool		suffixes = false;
	bool		prefixes = false;
	char		flagflags = 0;
	tsearch_readline_state trst;
	bool		oldformat = false;
	char	   *recoded = NULL;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	Conf->usecompound = false;
	Conf->useFlagAliases = false;
	Conf->flagMode = FM_CHAR;

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		pstr = lowerstr(recoded);

		/* Skip comments and empty lines */
		if (*pstr == '#' || *pstr == '\n')
			goto nextline;

		if (STRNCMP(pstr, "compoundwords") == 0)
		{
			/* Find case-insensitive L flag in non-lowercased string */
			s = findchar2(recoded, 'l', 'L');
			if (s)
			{
				while (*s && !t_isspace(s))
					s += pg_mblen(s);
				while (*s && t_isspace(s))
					s += pg_mblen(s);

				if (*s && pg_mblen(s) == 1)
				{
					addCompoundAffixFlagValue(Conf, s, FF_COMPOUNDFLAG);
					Conf->usecompound = true;
				}
				oldformat = true;
				goto nextline;
			}
		}
		if (STRNCMP(pstr, "suffixes") == 0)
		{
			suffixes = true;
			prefixes = false;
			oldformat = true;
			goto nextline;
		}
		if (STRNCMP(pstr, "prefixes") == 0)
		{
			suffixes = false;
			prefixes = true;
			oldformat = true;
			goto nextline;
		}
		if (STRNCMP(pstr, "flag") == 0)
		{
			s = recoded + 4;	/* we need non-lowercased string */
			flagflags = 0;

			while (*s && t_isspace(s))
				s += pg_mblen(s);

			if (*s == '*')
			{
				flagflags |= FF_CROSSPRODUCT;
				s++;
			}
			else if (*s == '~')
			{
				flagflags |= FF_COMPOUNDONLY;
				s++;
			}

			if (*s == '\\')
				s++;

			/*
			 * An old-format flag is a single ASCII character; we expect it to
			 * be followed by EOL, whitespace, or ':'.  Otherwise this is a
			 * new-format flag command.
			 */
			if (*s && pg_mblen(s) == 1)
			{
				COPYCHAR(flag, s);
				flag[1] = '\0';

				s++;
				if (*s == '\0' || *s == '#' || *s == '\n' || *s == ':' ||
					t_isspace(s))
				{
					oldformat = true;
					goto nextline;
				}
			}
			goto isnewformat;
		}
		if (STRNCMP(recoded, "COMPOUNDFLAG") == 0 ||
			STRNCMP(recoded, "COMPOUNDMIN") == 0 ||
			STRNCMP(recoded, "PFX") == 0 ||
			STRNCMP(recoded, "SFX") == 0)
			goto isnewformat;

		if ((!suffixes) && (!prefixes))
			goto nextline;

		if (!parse_affentry(pstr, mask, find, repl))
			goto nextline;

		NIAddAffix(Conf, flag, flagflags, mask, find, repl, suffixes ? FF_SUFFIX : FF_PREFIX);

nextline:
		pfree(recoded);
		pfree(pstr);
	}
	tsearch_readline_end(&trst);
	return;

isnewformat:
	if (oldformat)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("affix file contains both old-style and new-style commands")));
	tsearch_readline_end(&trst);

	NIImportOOAffixes(Conf, filename);
}

/*
 * Merges two affix flag sets and stores a new affix flag set into
 * Conf->AffixData.
 *
 * Returns index of a new affix flag set.
 */
static int
MergeAffix(IspellDict *Conf, int a1, int a2)
{
	const char **ptr;

	Assert(a1 < Conf->nAffixData && a2 < Conf->nAffixData);

	/* Do not merge affix flags if one of affix flags is empty */
	if (*Conf->AffixData[a1] == '\0')
		return a2;
	else if (*Conf->AffixData[a2] == '\0')
		return a1;

	/* Double the size of AffixData if there's not enough space */
	if (Conf->nAffixData + 1 >= Conf->lenAffixData)
	{
		Conf->lenAffixData *= 2;
		Conf->AffixData = (const char **) repalloc(Conf->AffixData,
												   sizeof(char *) * Conf->lenAffixData);
	}

	ptr = Conf->AffixData + Conf->nAffixData;
	if (Conf->flagMode == FM_NUM)
	{
		char	   *p = cpalloc(strlen(Conf->AffixData[a1]) +
								strlen(Conf->AffixData[a2]) +
								1 /* comma */ + 1 /* \0 */ );

		sprintf(p, "%s,%s", Conf->AffixData[a1], Conf->AffixData[a2]);
		*ptr = p;
	}
	else
	{
		char	   *p = cpalloc(strlen(Conf->AffixData[a1]) +
								strlen(Conf->AffixData[a2]) +
								1 /* \0 */ );

		sprintf(p, "%s%s", Conf->AffixData[a1], Conf->AffixData[a2]);
		*ptr = p;
	}
	ptr++;
	*ptr = NULL;
	Conf->nAffixData++;

	return Conf->nAffixData - 1;
}

/*
 * Returns a set of affix parameters which correspondence to the set of affix
 * flags with the given index.
 */
static uint32
makeCompoundFlags(IspellDict *Conf, int affix)
{
	Assert(affix < Conf->nAffixData);

	return (getCompoundAffixFlagValue(Conf, Conf->AffixData[affix]) &
			FF_COMPOUNDFLAGMASK);
}

/*
 * Makes a prefix tree for the given level.
 *
 * Conf: current dictionary.
 * low: lower index of the Conf->Spell array.
 * high: upper index of the Conf->Spell array.
 * level: current prefix tree level.
 */
static SPNode *
mkSPNode(IspellDict *Conf, int low, int high, int level)
{
	int			i;
	int			nchar = 0;
	char		lastchar = '\0';
	SPNode	   *rs;
	SPNodeData *data;
	int			lownew = low;

	for (i = low; i < high; i++)
		if (Conf->Spell[i]->p.d.len > level && lastchar != Conf->Spell[i]->word[level])
		{
			nchar++;
			lastchar = Conf->Spell[i]->word[level];
		}

	if (!nchar)
		return NULL;

	rs = (SPNode *) cpalloc0(SPNHDRSZ + nchar * sizeof(SPNodeData));
	rs->length = nchar;
	data = rs->data;

	lastchar = '\0';
	for (i = low; i < high; i++)
		if (Conf->Spell[i]->p.d.len > level)
		{
			if (lastchar != Conf->Spell[i]->word[level])
			{
				if (lastchar)
				{
					/* Next level of the prefix tree */
					data->node = mkSPNode(Conf, lownew, i, level + 1);
					lownew = i;
					data++;
				}
				lastchar = Conf->Spell[i]->word[level];
			}
			data->val = ((uint8 *) (Conf->Spell[i]->word))[level];
			if (Conf->Spell[i]->p.d.len == level + 1)
			{
				bool		clearCompoundOnly = false;

				if (data->isword && data->affix != Conf->Spell[i]->p.d.affix)
				{
					/*
					 * MergeAffix called a few times. If one of word is
					 * allowed to be in compound word and another isn't, then
					 * clear FF_COMPOUNDONLY flag.
					 */

					clearCompoundOnly = (FF_COMPOUNDONLY & data->compoundflag
										 & makeCompoundFlags(Conf, Conf->Spell[i]->p.d.affix))
						? false : true;
					data->affix = MergeAffix(Conf, data->affix, Conf->Spell[i]->p.d.affix);
				}
				else
					data->affix = Conf->Spell[i]->p.d.affix;
				data->isword = 1;

				data->compoundflag = makeCompoundFlags(Conf, data->affix);

				if ((data->compoundflag & FF_COMPOUNDONLY) &&
					(data->compoundflag & FF_COMPOUNDFLAG) == 0)
					data->compoundflag |= FF_COMPOUNDFLAG;

				if (clearCompoundOnly)
					data->compoundflag &= ~FF_COMPOUNDONLY;
			}
		}

	/* Next level of the prefix tree */
	data->node = mkSPNode(Conf, lownew, high, level + 1);

	return rs;
}

/*
 * Builds the Conf->Dictionary tree and AffixData from the imported dictionary
 * and affixes.
 */
void
NISortDictionary(IspellDict *Conf)
{
	int			i;
	int			naffix;
	int			curaffix;

	/* compress affixes */

	/*
	 * If we use flag aliases then we need to use Conf->AffixData filled in
	 * the NIImportOOAffixes().
	 */
	if (Conf->useFlagAliases)
	{
		for (i = 0; i < Conf->nspell; i++)
		{
			char	   *end;

			if (*Conf->Spell[i]->p.flag != '\0')
			{
				curaffix = strtol(Conf->Spell[i]->p.flag, &end, 10);
				if (Conf->Spell[i]->p.flag == end || errno == ERANGE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix alias \"%s\"",
									Conf->Spell[i]->p.flag)));
				if (curaffix < 0 || curaffix >= Conf->nAffixData)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix alias \"%s\"",
									Conf->Spell[i]->p.flag)));
				if (*end != '\0' && !t_isdigit(end) && !t_isspace(end))
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix alias \"%s\"",
									Conf->Spell[i]->p.flag)));
			}
			else
			{
				/*
				 * If Conf->Spell[i]->p.flag is empty, then get empty value of
				 * Conf->AffixData (0 index).
				 */
				curaffix = 0;
			}

			Conf->Spell[i]->p.d.affix = curaffix;
			Conf->Spell[i]->p.d.len = strlen(Conf->Spell[i]->word);
		}
	}
	/* Otherwise fill Conf->AffixData here */
	else
	{
		/* Count the number of different flags used in the dictionary */
		qsort(Conf->Spell, Conf->nspell, sizeof(SPELL *),
			  cmpspellaffix);

		naffix = 0;
		for (i = 0; i < Conf->nspell; i++)
		{
			if (i == 0 ||
				strcmp(Conf->Spell[i]->p.flag, Conf->Spell[i - 1]->p.flag) != 0)
				naffix++;
		}

		/*
		 * Fill in Conf->AffixData with the affixes that were used in the
		 * dictionary. Replace textual flag-field of Conf->Spell entries with
		 * indexes into Conf->AffixData array.
		 */
		Conf->AffixData = (const char **) palloc0(naffix * sizeof(const char *));

		curaffix = -1;
		for (i = 0; i < Conf->nspell; i++)
		{
			if (i == 0 ||
				strcmp(Conf->Spell[i]->p.flag, Conf->AffixData[curaffix]) != 0)
			{
				curaffix++;
				Assert(curaffix < naffix);
				Conf->AffixData[curaffix] = cpstrdup(Conf,
													 Conf->Spell[i]->p.flag);
			}

			Conf->Spell[i]->p.d.affix = curaffix;
			Conf->Spell[i]->p.d.len = strlen(Conf->Spell[i]->word);
		}

		Conf->lenAffixData = Conf->nAffixData = naffix;
	}

	/* Start build a prefix tree */
	qsort(Conf->Spell, Conf->nspell, sizeof(SPELL *), cmpspell);
	Conf->Dictionary = mkSPNode(Conf, 0, Conf->nspell, 0);
}

/*
 * Makes a prefix tree for the given level using the repl string of an affix
 * rule. Affixes with empty replace string do not include in the prefix tree.
 * This affixes are included by mkVoidAffix().
 *
 * Conf: current dictionary.
 * low: lower index of the Conf->Affix array.
 * high: upper index of the Conf->Affix array.
 * level: current prefix tree level.
 * type: FF_SUFFIX or FF_PREFIX.
 */
static AffixNode *
mkANode(IspellDict *Conf, int low, int high, int level, int type)
{
	int			i;
	int			nchar = 0;
	uint8		lastchar = '\0';
	AffixNode  *rs;
	AffixNodeData *data;
	int			lownew = low;
	int			naff;
	AFFIX	  **aff;

	for (i = low; i < high; i++)
		if (Conf->Affix[i].replen > level && lastchar != GETCHAR(Conf->Affix + i, level, type))
		{
			nchar++;
			lastchar = GETCHAR(Conf->Affix + i, level, type);
		}

	if (!nchar)
		return NULL;

	aff = (AFFIX **) tmpalloc(sizeof(AFFIX *) * (high - low + 1));
	naff = 0;

	rs = (AffixNode *) cpalloc0(ANHRDSZ + nchar * sizeof(AffixNodeData));
	rs->length = nchar;
	data = rs->data;

	lastchar = '\0';
	for (i = low; i < high; i++)
		if (Conf->Affix[i].replen > level)
		{
			if (lastchar != GETCHAR(Conf->Affix + i, level, type))
			{
				if (lastchar)
				{
					/* Next level of the prefix tree */
					data->node = mkANode(Conf, lownew, i, level + 1, type);
					if (naff)
					{
						data->naff = naff;
						data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * naff);
						memcpy(data->aff, aff, sizeof(AFFIX *) * naff);
						naff = 0;
					}
					data++;
					lownew = i;
				}
				lastchar = GETCHAR(Conf->Affix + i, level, type);
			}
			data->val = GETCHAR(Conf->Affix + i, level, type);
			if (Conf->Affix[i].replen == level + 1)
			{					/* affix stopped */
				aff[naff++] = Conf->Affix + i;
			}
		}

	/* Next level of the prefix tree */
	data->node = mkANode(Conf, lownew, high, level + 1, type);
	if (naff)
	{
		data->naff = naff;
		data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * naff);
		memcpy(data->aff, aff, sizeof(AFFIX *) * naff);
		naff = 0;
	}

	pfree(aff);

	return rs;
}

/*
 * Makes the root void node in the prefix tree. The root void node is created
 * for affixes which have empty replace string ("repl" field).
 */
static void
mkVoidAffix(IspellDict *Conf, bool issuffix, int startsuffix)
{
	int			i,
				cnt = 0;
	int			start = (issuffix) ? startsuffix : 0;
	int			end = (issuffix) ? Conf->naffixes : startsuffix;
	AffixNode  *Affix = (AffixNode *) palloc0(ANHRDSZ + sizeof(AffixNodeData));

	Affix->length = 1;
	Affix->isvoid = 1;

	if (issuffix)
	{
		Affix->data->node = Conf->Suffix;
		Conf->Suffix = Affix;
	}
	else
	{
		Affix->data->node = Conf->Prefix;
		Conf->Prefix = Affix;
	}

	/* Count affixes with empty replace string */
	for (i = start; i < end; i++)
		if (Conf->Affix[i].replen == 0)
			cnt++;

	/* There is not affixes with empty replace string */
	if (cnt == 0)
		return;

	Affix->data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * cnt);
	Affix->data->naff = (uint32) cnt;

	cnt = 0;
	for (i = start; i < end; i++)
		if (Conf->Affix[i].replen == 0)
		{
			Affix->data->aff[cnt] = Conf->Affix + i;
			cnt++;
		}
}

/*
 * Checks if the affixflag is used by dictionary. Conf->AffixData does not
 * contain affixflag if this flag is not used actually by the .dict file.
 *
 * Conf: current dictionary.
 * affixflag: affix flag.
 *
 * Returns true if the Conf->AffixData array contains affixflag, otherwise
 * returns false.
 */
static bool
isAffixInUse(IspellDict *Conf, const char *affixflag)
{
	int			i;

	for (i = 0; i < Conf->nAffixData; i++)
		if (IsAffixFlagInUse(Conf, i, affixflag))
			return true;

	return false;
}

/*
 * Builds Conf->Prefix and Conf->Suffix trees from the imported affixes.
 */
void
NISortAffixes(IspellDict *Conf)
{
	AFFIX	   *Affix;
	size_t		i;
	CMPDAffix  *ptr;
	int			firstsuffix = Conf->naffixes;

	if (Conf->naffixes == 0)
		return;

	/* Store compound affixes in the Conf->CompoundAffix array */
	if (Conf->naffixes > 1)
		qsort(Conf->Affix, Conf->naffixes, sizeof(AFFIX), cmpaffix);
	Conf->CompoundAffix = ptr = (CMPDAffix *) palloc(sizeof(CMPDAffix) * Conf->naffixes);
	ptr->affix = NULL;

	for (i = 0; i < Conf->naffixes; i++)
	{
		Affix = &(((AFFIX *) Conf->Affix)[i]);
		if (Affix->type == FF_SUFFIX && i < firstsuffix)
			firstsuffix = i;

		if ((Affix->flagflags & FF_COMPOUNDFLAG) && Affix->replen > 0 &&
			isAffixInUse(Conf, Affix->flag))
		{
			bool		issuffix = (Affix->type == FF_SUFFIX);

			if (ptr == Conf->CompoundAffix ||
				issuffix != (ptr - 1)->issuffix ||
				strbncmp((const unsigned char *) (ptr - 1)->affix,
						 (const unsigned char *) Affix->repl,
						 (ptr - 1)->len))
			{
				/* leave only unique and minimal suffixes */
				ptr->affix = Affix->repl;
				ptr->len = Affix->replen;
				ptr->issuffix = issuffix;
				ptr++;
			}
		}
	}
	ptr->affix = NULL;
	Conf->CompoundAffix = (CMPDAffix *) repalloc(Conf->CompoundAffix, sizeof(CMPDAffix) * (ptr - Conf->CompoundAffix + 1));

	/* Start build a prefix tree */
	Conf->Prefix = mkANode(Conf, 0, firstsuffix, 0, FF_PREFIX);
	Conf->Suffix = mkANode(Conf, firstsuffix, Conf->naffixes, 0, FF_SUFFIX);
	mkVoidAffix(Conf, true, firstsuffix);
	mkVoidAffix(Conf, false, firstsuffix);
}

static AffixNodeData *
FindAffixes(AffixNode *node, const char *word, int wrdlen, int *level, int type)
{
	AffixNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	uint8 symbol;

	if (node->isvoid)
	{							/* search void affixes */
		if (node->data->naff)
			return node->data;
		node = node->data->node;
	}

	while (node && *level < wrdlen)
	{
		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			symbol = GETWCHAR(word, wrdlen, *level, type);

			if (StopMiddle->val == symbol)
			{
				(*level)++;
				if (StopMiddle->naff)
					return StopMiddle;
				node = StopMiddle->node;
				break;
			}
			else if (StopMiddle->val < symbol)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
		if (StopLow >= StopHigh)
			break;
	}
	return NULL;
}

static char *
CheckAffix(const char *word, size_t len, AFFIX *Affix, int flagflags, char *newword, int *baselen)
{
	/*
	 * Check compound allow flags
	 */

	if (flagflags == 0)
	{
		if (Affix->flagflags & FF_COMPOUNDONLY)
			return NULL;
	}
	else if (flagflags & FF_COMPOUNDBEGIN)
	{
		if (Affix->flagflags & FF_COMPOUNDFORBIDFLAG)
			return NULL;
		if ((Affix->flagflags & FF_COMPOUNDBEGIN) == 0)
			if (Affix->type == FF_SUFFIX)
				return NULL;
	}
	else if (flagflags & FF_COMPOUNDMIDDLE)
	{
		if ((Affix->flagflags & FF_COMPOUNDMIDDLE) == 0 ||
			(Affix->flagflags & FF_COMPOUNDFORBIDFLAG))
			return NULL;
	}
	else if (flagflags & FF_COMPOUNDLAST)
	{
		if (Affix->flagflags & FF_COMPOUNDFORBIDFLAG)
			return NULL;
		if ((Affix->flagflags & FF_COMPOUNDLAST) == 0)
			if (Affix->type == FF_PREFIX)
				return NULL;
	}

	/*
	 * make replace pattern of affix
	 */
	if (Affix->type == FF_SUFFIX)
	{
		strcpy(newword, word);
		strcpy(newword + len - Affix->replen, Affix->find);
		if (baselen)			/* store length of non-changed part of word */
			*baselen = len - Affix->replen;
	}
	else
	{
		/*
		 * if prefix is an all non-changed part's length then all word
		 * contains only prefix and suffix, so out
		 */
		if (baselen && *baselen + strlen(Affix->find) <= Affix->replen)
			return NULL;
		strcpy(newword, Affix->find);
		strcat(newword, word + Affix->replen);
	}

	/*
	 * check resulting word
	 */
	if (Affix->issimple)
		return newword;
	else if (Affix->isregis)
	{
		if (RS_execute(&(Affix->reg.regis), newword))
			return newword;
	}
	else
	{
		pg_wchar   *data;
		size_t		data_len;
		int			newword_len;

		/* Convert data string to wide characters */
		newword_len = strlen(newword);
		data = (pg_wchar *) palloc((newword_len + 1) * sizeof(pg_wchar));
		data_len = pg_mb2wchar_with_len(newword, data, newword_len);

		if (pg_regexec(Affix->reg.pregex, data, data_len,
					   0, NULL, 0, NULL, 0) == REG_OKAY)
		{
			pfree(data);
			return newword;
		}
		pfree(data);
	}

	return NULL;
}

static int
addToResult(char **forms, char **cur, char *word)
{
	if (cur - forms >= MAX_NORM - 1)
		return 0;
	if (forms == cur || strcmp(word, *(cur - 1)) != 0)
	{
		*cur = pstrdup(word);
		*(cur + 1) = NULL;
		return 1;
	}

	return 0;
}

static char **
NormalizeSubWord(IspellDict *Conf, const char *word, int flag)
{
	AffixNodeData *suffix = NULL,
			   *prefix = NULL;
	int			slevel = 0,
				plevel = 0;
	int			wrdlen = strlen(word),
				swrdlen;
	char	  **forms;
	char	  **cur;
	char		newword[2 * MAXNORMLEN] = "";
	char		pnewword[2 * MAXNORMLEN] = "";
	AffixNode  *snode = Conf->Suffix,
			   *pnode;
	int			i,
				j;

	if (wrdlen > MAXNORMLEN)
		return NULL;
	cur = forms = (char **) palloc(MAX_NORM * sizeof(char *));
	*cur = NULL;


	/* Check that the word itself is normal form */
	if (FindWord(Conf, word, VoidString, flag))
	{
		*cur = pstrdup(word);
		cur++;
		*cur = NULL;
	}

	/* Find all other NORMAL forms of the 'word' (check only prefix) */
	pnode = Conf->Prefix;
	plevel = 0;
	while (pnode)
	{
		prefix = FindAffixes(pnode, word, wrdlen, &plevel, FF_PREFIX);
		if (!prefix)
			break;
		for (j = 0; j < prefix->naff; j++)
		{
			if (CheckAffix(word, wrdlen, prefix->aff[j], flag, newword, NULL))
			{
				/* prefix success */
				if (FindWord(Conf, newword, prefix->aff[j]->flag, flag))
					cur += addToResult(forms, cur, newword);
			}
		}
		pnode = prefix->node;
	}

	/*
	 * Find all other NORMAL forms of the 'word' (check suffix and then
	 * prefix)
	 */
	while (snode)
	{
		int			baselen = 0;

		/* find possible suffix */
		suffix = FindAffixes(snode, word, wrdlen, &slevel, FF_SUFFIX);
		if (!suffix)
			break;
		/* foreach suffix check affix */
		for (i = 0; i < suffix->naff; i++)
		{
			if (CheckAffix(word, wrdlen, suffix->aff[i], flag, newword, &baselen))
			{
				/* suffix success */
				if (FindWord(Conf, newword, suffix->aff[i]->flag, flag))
					cur += addToResult(forms, cur, newword);

				/* now we will look changed word with prefixes */
				pnode = Conf->Prefix;
				plevel = 0;
				swrdlen = strlen(newword);
				while (pnode)
				{
					prefix = FindAffixes(pnode, newword, swrdlen, &plevel, FF_PREFIX);
					if (!prefix)
						break;
					for (j = 0; j < prefix->naff; j++)
					{
						if (CheckAffix(newword, swrdlen, prefix->aff[j], flag, pnewword, &baselen))
						{
							/* prefix success */
							const char *ff = (prefix->aff[j]->flagflags & suffix->aff[i]->flagflags & FF_CROSSPRODUCT) ?
								VoidString : prefix->aff[j]->flag;

							if (FindWord(Conf, pnewword, ff, flag))
								cur += addToResult(forms, cur, pnewword);
						}
					}
					pnode = prefix->node;
				}
			}
		}

		snode = suffix->node;
	}

	if (cur == forms)
	{
		pfree(forms);
		return NULL;
	}
	return forms;
}

typedef struct SplitVar
{
	int			nstem;
	int			lenstem;
	char	  **stem;
	struct SplitVar *next;
} SplitVar;

static int
CheckCompoundAffixes(CMPDAffix **ptr, const char *word, int len, bool CheckInPlace)
{
	bool		issuffix;

	/* in case CompoundAffix is null: */
	if (*ptr == NULL)
		return -1;

	if (CheckInPlace)
	{
		while ((*ptr)->affix)
		{
			if (len > (*ptr)->len && strncmp((*ptr)->affix, word, (*ptr)->len) == 0)
			{
				len = (*ptr)->len;
				issuffix = (*ptr)->issuffix;
				(*ptr)++;
				return (issuffix) ? len : 0;
			}
			(*ptr)++;
		}
	}
	else
	{
		char	   *affbegin;

		while ((*ptr)->affix)
		{
			if (len > (*ptr)->len && (affbegin = strstr(word, (*ptr)->affix)) != NULL)
			{
				len = (*ptr)->len + (affbegin - word);
				issuffix = (*ptr)->issuffix;
				(*ptr)++;
				return (issuffix) ? len : 0;
			}
			(*ptr)++;
		}
	}
	return -1;
}

static SplitVar *
CopyVar(SplitVar *s, int makedup)
{
	SplitVar   *v = (SplitVar *) palloc(sizeof(SplitVar));

	v->next = NULL;
	if (s)
	{
		int			i;

		v->lenstem = s->lenstem;
		v->stem = (char **) palloc(sizeof(char *) * v->lenstem);
		v->nstem = s->nstem;
		for (i = 0; i < s->nstem; i++)
			v->stem[i] = (makedup) ? pstrdup(s->stem[i]) : s->stem[i];
	}
	else
	{
		v->lenstem = 16;
		v->stem = (char **) palloc(sizeof(char *) * v->lenstem);
		v->nstem = 0;
	}
	return v;
}

static void
AddStem(SplitVar *v, char *word)
{
	if (v->nstem >= v->lenstem)
	{
		v->lenstem *= 2;
		v->stem = (char **) repalloc(v->stem, sizeof(char *) * v->lenstem);
	}

	v->stem[v->nstem] = word;
	v->nstem++;
}

static SplitVar *
SplitToVariants(IspellDict *Conf, SPNode *snode, SplitVar *orig, const char *word, int wordlen, int startpos, int minpos)
{
	SplitVar   *var = NULL;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle = NULL;
	SPNode	   *node = (snode) ? snode : Conf->Dictionary;
	int			level = (snode) ? minpos : startpos;	/* recursive
														 * minpos==level */
	int			lenaff;
	CMPDAffix  *caff;
	char	   *notprobed;
	int			compoundflag = 0;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	notprobed = (char *) palloc(wordlen);
	memset(notprobed, 1, wordlen);
	var = CopyVar(orig, 1);

	while (level < wordlen)
	{
		/* find word with epenthetic or/and compound affix */
		caff = Conf->CompoundAffix;
		while (level > startpos && (lenaff = CheckCompoundAffixes(&caff, word + level, wordlen - level, (node) ? true : false)) >= 0)
		{
			/*
			 * there is one of compound affixes, so check word for existings
			 */
			char		buf[MAXNORMLEN];
			char	  **subres;

			lenaff = level - startpos + lenaff;

			if (!notprobed[startpos + lenaff - 1])
				continue;

			if (level + lenaff - 1 <= minpos)
				continue;

			if (lenaff >= MAXNORMLEN)
				continue;		/* skip too big value */
			if (lenaff > 0)
				memcpy(buf, word + startpos, lenaff);
			buf[lenaff] = '\0';

			if (level == 0)
				compoundflag = FF_COMPOUNDBEGIN;
			else if (level == wordlen - 1)
				compoundflag = FF_COMPOUNDLAST;
			else
				compoundflag = FF_COMPOUNDMIDDLE;
			subres = NormalizeSubWord(Conf, buf, compoundflag);
			if (subres)
			{
				/* Yes, it was a word from dictionary */
				SplitVar   *new = CopyVar(var, 0);
				SplitVar   *ptr = var;
				char	  **sptr = subres;

				notprobed[startpos + lenaff - 1] = 0;

				while (*sptr)
				{
					AddStem(new, *sptr);
					sptr++;
				}
				pfree(subres);

				while (ptr->next)
					ptr = ptr->next;
				ptr->next = SplitToVariants(Conf, NULL, new, word, wordlen, startpos + lenaff, startpos + lenaff);

				pfree(new->stem);
				pfree(new);
			}
		}

		if (!node)
			break;

		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (StopMiddle->val == ((uint8 *) (word))[level])
				break;
			else if (StopMiddle->val < ((uint8 *) (word))[level])
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}

		if (StopLow < StopHigh)
		{
			if (startpos == 0)
				compoundflag = FF_COMPOUNDBEGIN;
			else if (level == wordlen - 1)
				compoundflag = FF_COMPOUNDLAST;
			else
				compoundflag = FF_COMPOUNDMIDDLE;

			/* find infinitive */
			if (StopMiddle->isword &&
				(StopMiddle->compoundflag & compoundflag) &&
				notprobed[level])
			{
				/* ok, we found full compoundallowed word */
				if (level > minpos)
				{
					/* and its length more than minimal */
					if (wordlen == level + 1)
					{
						/* well, it was last word */
						AddStem(var, pnstrdup(word + startpos, wordlen - startpos));
						pfree(notprobed);
						return var;
					}
					else
					{
						/* then we will search more big word at the same point */
						SplitVar   *ptr = var;

						while (ptr->next)
							ptr = ptr->next;
						ptr->next = SplitToVariants(Conf, node, var, word, wordlen, startpos, level);
						/* we can find next word */
						level++;
						AddStem(var, pnstrdup(word + startpos, level - startpos));
						node = Conf->Dictionary;
						startpos = level;
						continue;
					}
				}
			}
			node = StopMiddle->node;
		}
		else
			node = NULL;
		level++;
	}

	AddStem(var, pnstrdup(word + startpos, wordlen - startpos));
	pfree(notprobed);
	return var;
}

static void
addNorm(TSLexeme **lres, TSLexeme **lcur, char *word, int flags, uint16 NVariant)
{
	if (*lres == NULL)
		*lcur = *lres = (TSLexeme *) palloc(MAX_NORM * sizeof(TSLexeme));

	if (*lcur - *lres < MAX_NORM - 1)
	{
		(*lcur)->lexeme = word;
		(*lcur)->flags = flags;
		(*lcur)->nvariant = NVariant;
		(*lcur)++;
		(*lcur)->lexeme = NULL;
	}
}

TSLexeme *
NINormalizeWord(IspellDict *Conf, const char *word)
{
	char	  **res;
	TSLexeme   *lcur = NULL,
			   *lres = NULL;
	uint16		NVariant = 1;

	res = NormalizeSubWord(Conf, word, 0);

	if (res)
	{
		char	  **ptr = res;

		while (*ptr && (lcur - lres) < MAX_NORM)
		{
			addNorm(&lres, &lcur, *ptr, 0, NVariant++);
			ptr++;
		}
		pfree(res);
	}

	if (Conf->usecompound)
	{
		int			wordlen = strlen(word);
		SplitVar   *ptr,
				   *var = SplitToVariants(Conf, NULL, NULL, word, wordlen, 0, -1);
		int			i;

		while (var)
		{
			if (var->nstem > 1)
			{
				char	  **subres = NormalizeSubWord(Conf, var->stem[var->nstem - 1], FF_COMPOUNDLAST);

				if (subres)
				{
					char	  **subptr = subres;

					while (*subptr)
					{
						for (i = 0; i < var->nstem - 1; i++)
						{
							addNorm(&lres, &lcur, (subptr == subres) ? var->stem[i] : pstrdup(var->stem[i]), 0, NVariant);
						}

						addNorm(&lres, &lcur, *subptr, 0, NVariant);
						subptr++;
						NVariant++;
					}

					pfree(subres);
					var->stem[0] = NULL;
					pfree(var->stem[var->nstem - 1]);
				}
			}

			for (i = 0; i < var->nstem && var->stem[i]; i++)
				pfree(var->stem[i]);
			ptr = var->next;
			pfree(var->stem);
			pfree(var);
			var = ptr;
		}
	}

	return lres;
}
