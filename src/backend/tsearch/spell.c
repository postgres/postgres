/*-------------------------------------------------------------------------
 *
 * spell.c
 *		Normalizing word with ISpell
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/spell.c,v 1.16 2009/06/11 14:49:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"


/*
 * Initialization requires a lot of memory that's not needed
 * after the initialization is done.  In init function,
 * CurrentMemoryContext is a long lived memory context associated
 * with the dictionary cache entry, so we use a temporary context
 * for the short-lived stuff.
 */
static MemoryContext tmpCtx = NULL;

#define tmpalloc(sz)  MemoryContextAlloc(tmpCtx, (sz))
#define tmpalloc0(sz)  MemoryContextAllocZero(tmpCtx, (sz))

static void
checkTmpCtx(void)
{
	/*
	 * XXX: This assumes that CurrentMemoryContext doesn't have any children
	 * other than the one we create here.
	 */
	if (CurrentMemoryContext->firstchild == NULL)
	{
		tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Ispell dictionary init context",
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);
	}
	else
		tmpCtx = CurrentMemoryContext->firstchild;
}

static char *
lowerstr_ctx(char *src)
{
	MemoryContext saveCtx;
	char	   *dst;

	saveCtx = MemoryContextSwitchTo(tmpCtx);
	dst = lowerstr(src);
	MemoryContextSwitchTo(saveCtx);

	return dst;
}

#define MAX_NORM 1024
#define MAXNORMLEN 256

#define STRNCMP(s,p)	strncmp( (s), (p), strlen(p) )
#define GETWCHAR(W,L,N,T) ( ((uint8*)(W))[ ((T)==FF_PREFIX) ? (N) : ( (L) - 1 - (N) ) ] )
#define GETCHAR(A,N,T)	  GETWCHAR( (A)->repl, (A)->replen, N, T )

static char *VoidString = "";

static int
cmpspell(const void *s1, const void *s2)
{
	return (strcmp((*(const SPELL **) s1)->word, (*(const SPELL **) s2)->word));
}
static int
cmpspellaffix(const void *s1, const void *s2)
{
	return (strncmp((*(const SPELL **) s1)->p.flag, (*(const SPELL **) s2)->p.flag, MAXFLAGLEN));
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

static void
NIAddSpell(IspellDict *Conf, const char *word, const char *flag)
{
	if (Conf->nspell >= Conf->mspell)
	{
		if (Conf->mspell)
		{
			Conf->mspell += 1024 * 20;
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
	strlcpy(Conf->Spell[Conf->nspell]->p.flag, flag, MAXFLAGLEN);
	Conf->nspell++;
}

/*
 * import dictionary
 *
 * Note caller must already have applied get_tsearch_config_filename
 */
void
NIImportDictionary(IspellDict *Conf, const char *filename)
{
	tsearch_readline_state trst;
	char	   *line;

	checkTmpCtx();

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open dictionary file \"%s\": %m",
						filename)));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		char	   *s,
				   *pstr;
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
		pstr = lowerstr_ctx(line);

		NIAddSpell(Conf, pstr, flag);
		pfree(pstr);

		pfree(line);
	}
	tsearch_readline_end(&trst);
}


static int
FindWord(IspellDict *Conf, const char *word, int affixflag, int flag)
{
	SPNode	   *node = Conf->Dictionary;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	uint8	   *ptr = (uint8 *) word;

	flag &= FF_DICTFLAGMASK;

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
						if (StopMiddle->compoundflag & FF_COMPOUNDONLY)
							return 0;
					}
					else if ((flag & StopMiddle->compoundflag) == 0)
						return 0;

					if ((affixflag == 0) || (strchr(Conf->AffixData[StopMiddle->affix], affixflag) != NULL))
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

static void
NIAddAffix(IspellDict *Conf, int flag, char flagflags, const char *mask, const char *find, const char *repl, int type)
{
	AFFIX	   *Affix;

	if (Conf->naffixes >= Conf->maffixes)
	{
		if (Conf->maffixes)
		{
			Conf->maffixes += 16;
			Conf->Affix = (AFFIX *) repalloc((void *) Conf->Affix, Conf->maffixes * sizeof(AFFIX));
		}
		else
		{
			Conf->maffixes = 16;
			Conf->Affix = (AFFIX *) palloc(Conf->maffixes * sizeof(AFFIX));
		}
	}

	Affix = Conf->Affix + Conf->naffixes;

	if (strcmp(mask, ".") == 0)
	{
		Affix->issimple = 1;
		Affix->isregis = 0;
	}
	else if (RS_isRegis(mask))
	{
		Affix->issimple = 0;
		Affix->isregis = 1;
		RS_compile(&(Affix->reg.regis), (type == FF_SUFFIX) ? true : false,
				   (mask && *mask) ? mask : VoidString);
	}
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

		err = pg_regcomp(&(Affix->reg.regex), wmask, wmasklen, REG_ADVANCED | REG_NOSUB);
		if (err)
		{
			char		errstr[100];

			pg_regerror(err, &(Affix->reg.regex), errstr, sizeof(errstr));
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
	Affix->flag = flag;
	Affix->type = type;

	Affix->find = (find && *find) ? pstrdup(find) : VoidString;
	if ((Affix->replen = strlen(repl)) > 0)
		Affix->repl = pstrdup(repl);
	else
		Affix->repl = VoidString;
	Conf->naffixes++;
}

#define PAE_WAIT_MASK	0
#define PAE_INMASK	1
#define PAE_WAIT_FIND	2
#define PAE_INFIND	3
#define PAE_WAIT_REPL	4
#define PAE_INREPL	5

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

	return (*mask && (*find || *repl)) ? true : false;
}

static void
addFlagValue(IspellDict *Conf, char *s, uint32 val)
{
	while (*s && t_isspace(s))
		s += pg_mblen(s);

	if (!*s)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("syntax error")));

	if (pg_mblen(s) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multibyte flag character is not allowed")));

	Conf->flagval[*(unsigned char *) s] = (unsigned char) val;
	Conf->usecompound = true;
}

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
	int			flag = 0;
	char		flagflags = 0;
	tsearch_readline_state trst;
	int			scanread = 0;
	char		scanbuf[BUFSIZ];
	char	   *recoded;

	checkTmpCtx();

	/* read file to find any flag */
	memset(Conf->flagval, 0, sizeof(Conf->flagval));
	Conf->usecompound = false;

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
			addFlagValue(Conf, recoded + strlen("COMPOUNDFLAG"),
						 FF_COMPOUNDFLAG);
		else if (STRNCMP(recoded, "COMPOUNDBEGIN") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDBEGIN"),
						 FF_COMPOUNDBEGIN);
		else if (STRNCMP(recoded, "COMPOUNDLAST") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDLAST"),
						 FF_COMPOUNDLAST);
		/* COMPOUNDLAST and COMPOUNDEND are synonyms */
		else if (STRNCMP(recoded, "COMPOUNDEND") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDEND"),
						 FF_COMPOUNDLAST);
		else if (STRNCMP(recoded, "COMPOUNDMIDDLE") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDMIDDLE"),
						 FF_COMPOUNDMIDDLE);
		else if (STRNCMP(recoded, "ONLYINCOMPOUND") == 0)
			addFlagValue(Conf, recoded + strlen("ONLYINCOMPOUND"),
						 FF_COMPOUNDONLY);
		else if (STRNCMP(recoded, "COMPOUNDPERMITFLAG") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDPERMITFLAG"),
						 FF_COMPOUNDPERMITFLAG);
		else if (STRNCMP(recoded, "COMPOUNDFORBIDFLAG") == 0)
			addFlagValue(Conf, recoded + strlen("COMPOUNDFORBIDFLAG"),
						 FF_COMPOUNDFORBIDFLAG);
		else if (STRNCMP(recoded, "FLAG") == 0)
		{
			char	   *s = recoded + strlen("FLAG");

			while (*s && t_isspace(s))
				s += pg_mblen(s);

			if (*s && STRNCMP(s, "default") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("Ispell dictionary supports only default flag value")));
		}

		pfree(recoded);
	}
	tsearch_readline_end(&trst);

	sprintf(scanbuf, "%%6s %%%ds %%%ds %%%ds %%%ds", BUFSIZ / 5, BUFSIZ / 5, BUFSIZ / 5, BUFSIZ / 5);

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		if (*recoded == '\0' || t_isspace(recoded) || t_iseq(recoded, '#'))
			goto nextline;

		scanread = sscanf(recoded, scanbuf, type, sflag, find, repl, mask);

		if (ptype)
			pfree(ptype);
		ptype = lowerstr_ctx(type);
		if (scanread < 4 || (STRNCMP(ptype, "sfx") && STRNCMP(ptype, "pfx")))
			goto nextline;

		if (scanread == 4)
		{
			if (strlen(sflag) != 1)
				goto nextline;
			flag = *sflag;
			isSuffix = (STRNCMP(ptype, "sfx") == 0) ? true : false;
			if (t_iseq(find, 'y') || t_iseq(find, 'Y'))
				flagflags = FF_CROSSPRODUCT;
			else
				flagflags = 0;
		}
		else
		{
			char	   *ptr;
			int			aflg = 0;

			if (strlen(sflag) != 1 || flag != *sflag || flag == 0)
				goto nextline;
			prepl = lowerstr_ctx(repl);
			/* affix flag */
			if ((ptr = strchr(prepl, '/')) != NULL)
			{
				*ptr = '\0';
				ptr = repl + (ptr - prepl) + 1;
				while (*ptr)
				{
					aflg |= Conf->flagval[*(unsigned char *) ptr];
					ptr++;
				}
			}
			pfind = lowerstr_ctx(find);
			pmask = lowerstr_ctx(mask);
			if (t_iseq(find, '0'))
				*pfind = '\0';
			if (t_iseq(repl, '0'))
				*prepl = '\0';

			NIAddAffix(Conf, flag, flagflags | aflg, pmask, pfind, prepl,
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
 */
void
NIImportAffixes(IspellDict *Conf, const char *filename)
{
	char	   *pstr = NULL;
	char		mask[BUFSIZ];
	char		find[BUFSIZ];
	char		repl[BUFSIZ];
	char	   *s;
	bool		suffixes = false;
	bool		prefixes = false;
	int			flag = 0;
	char		flagflags = 0;
	tsearch_readline_state trst;
	bool		oldformat = false;
	char	   *recoded = NULL;

	checkTmpCtx();

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	memset(Conf->flagval, 0, sizeof(Conf->flagval));
	Conf->usecompound = false;

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		pstr = lowerstr(recoded);

		/* Skip comments and empty lines */
		if (*pstr == '#' || *pstr == '\n')
			goto nextline;

		if (STRNCMP(pstr, "compoundwords") == 0)
		{
			s = findchar(pstr, 'l');
			if (s)
			{
				s = recoded + (s - pstr);		/* we need non-lowercased
												 * string */
				while (*s && !t_isspace(s))
					s += pg_mblen(s);
				while (*s && t_isspace(s))
					s += pg_mblen(s);

				if (*s && pg_mblen(s) == 1)
				{
					Conf->flagval[*(unsigned char *) s] = FF_COMPOUNDFLAG;
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
			oldformat = true;

			/* allow only single-encoded flags */
			if (pg_mblen(s) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("multibyte flag character is not allowed")));

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

			/* allow only single-encoded flags */
			if (pg_mblen(s) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("multibyte flag character is not allowed")));

			flag = *(unsigned char *) s;
			goto nextline;
		}
		if (STRNCMP(recoded, "COMPOUNDFLAG") == 0 || STRNCMP(recoded, "COMPOUNDMIN") == 0 ||
			STRNCMP(recoded, "PFX") == 0 || STRNCMP(recoded, "SFX") == 0)
		{
			if (oldformat)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("wrong affix file format for flag")));
			tsearch_readline_end(&trst);
			NIImportOOAffixes(Conf, filename);
			return;
		}
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
}

static int
MergeAffix(IspellDict *Conf, int a1, int a2)
{
	char	  **ptr;

	while (Conf->nAffixData + 1 >= Conf->lenAffixData)
	{
		Conf->lenAffixData *= 2;
		Conf->AffixData = (char **) repalloc(Conf->AffixData,
										sizeof(char *) * Conf->lenAffixData);
	}

	ptr = Conf->AffixData + Conf->nAffixData;
	*ptr = palloc(strlen(Conf->AffixData[a1]) + strlen(Conf->AffixData[a2]) +
				  1 /* space */ + 1 /* \0 */ );
	sprintf(*ptr, "%s %s", Conf->AffixData[a1], Conf->AffixData[a2]);
	ptr++;
	*ptr = NULL;
	Conf->nAffixData++;

	return Conf->nAffixData - 1;
}

static uint32
makeCompoundFlags(IspellDict *Conf, int affix)
{
	uint32		flag = 0;
	char	   *str = Conf->AffixData[affix];

	while (str && *str)
	{
		flag |= Conf->flagval[*(unsigned char *) str];
		str++;
	}

	return (flag & FF_DICTFLAGMASK);
}

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

	rs = (SPNode *) palloc0(SPNHDRSZ + nchar * sizeof(SPNodeData));
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
	int			naffix = 0;
	int			curaffix;

	checkTmpCtx();

	/* compress affixes */

	/* Count the number of different flags used in the dictionary */

	qsort((void *) Conf->Spell, Conf->nspell, sizeof(SPELL *), cmpspellaffix);

	naffix = 0;
	for (i = 0; i < Conf->nspell; i++)
	{
		if (i == 0 || strncmp(Conf->Spell[i]->p.flag, Conf->Spell[i - 1]->p.flag, MAXFLAGLEN))
			naffix++;
	}

	/*
	 * Fill in Conf->AffixData with the affixes that were used in the
	 * dictionary. Replace textual flag-field of Conf->Spell entries with
	 * indexes into Conf->AffixData array.
	 */
	Conf->AffixData = (char **) palloc0(naffix * sizeof(char *));

	curaffix = -1;
	for (i = 0; i < Conf->nspell; i++)
	{
		if (i == 0 || strncmp(Conf->Spell[i]->p.flag, Conf->AffixData[curaffix], MAXFLAGLEN))
		{
			curaffix++;
			Assert(curaffix < naffix);
			Conf->AffixData[curaffix] = pstrdup(Conf->Spell[i]->p.flag);
		}

		Conf->Spell[i]->p.d.affix = curaffix;
		Conf->Spell[i]->p.d.len = strlen(Conf->Spell[i]->word);
	}

	Conf->lenAffixData = Conf->nAffixData = naffix;

	qsort((void *) Conf->Spell, Conf->nspell, sizeof(SPELL *), cmpspell);
	Conf->Dictionary = mkSPNode(Conf, 0, Conf->nspell, 0);

	Conf->Spell = NULL;
}

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

	rs = (AffixNode *) palloc0(ANHRDSZ + nchar * sizeof(AffixNodeData));
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
					data->node = mkANode(Conf, lownew, i, level + 1, type);
					if (naff)
					{
						data->naff = naff;
						data->aff = (AFFIX **) palloc(sizeof(AFFIX *) * naff);
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

	data->node = mkANode(Conf, lownew, high, level + 1, type);
	if (naff)
	{
		data->naff = naff;
		data->aff = (AFFIX **) palloc(sizeof(AFFIX *) * naff);
		memcpy(data->aff, aff, sizeof(AFFIX *) * naff);
		naff = 0;
	}

	pfree(aff);

	return rs;
}

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


	for (i = start; i < end; i++)
		if (Conf->Affix[i].replen == 0)
			cnt++;

	if (cnt == 0)
		return;

	Affix->data->aff = (AFFIX **) palloc(sizeof(AFFIX *) * cnt);
	Affix->data->naff = (uint32) cnt;

	cnt = 0;
	for (i = start; i < end; i++)
		if (Conf->Affix[i].replen == 0)
		{
			Affix->data->aff[cnt] = Conf->Affix + i;
			cnt++;
		}
}

static bool
isAffixInUse(IspellDict *Conf, char flag)
{
	int			i;

	for (i = 0; i < Conf->nAffixData; i++)
		if (strchr(Conf->AffixData[i], flag) != NULL)
			return true;

	return false;
}

void
NISortAffixes(IspellDict *Conf)
{
	AFFIX	   *Affix;
	size_t		i;
	CMPDAffix  *ptr;
	int			firstsuffix = Conf->naffixes;

	checkTmpCtx();

	if (Conf->naffixes == 0)
		return;

	if (Conf->naffixes > 1)
		qsort((void *) Conf->Affix, Conf->naffixes, sizeof(AFFIX), cmpaffix);
	Conf->CompoundAffix = ptr = (CMPDAffix *) palloc(sizeof(CMPDAffix) * Conf->naffixes);
	ptr->affix = NULL;

	for (i = 0; i < Conf->naffixes; i++)
	{
		Affix = &(((AFFIX *) Conf->Affix)[i]);
		if (Affix->type == FF_SUFFIX && i < firstsuffix)
			firstsuffix = i;

		if ((Affix->flagflags & FF_COMPOUNDFLAG) && Affix->replen > 0 &&
			isAffixInUse(Conf, (char) Affix->flag))
		{
			if (ptr == Conf->CompoundAffix ||
				ptr->issuffix != (ptr - 1)->issuffix ||
				strbncmp((const unsigned char *) (ptr - 1)->affix,
						 (const unsigned char *) Affix->repl,
						 (ptr - 1)->len))
			{
				/* leave only unique and minimals suffixes */
				ptr->affix = Affix->repl;
				ptr->len = Affix->replen;
				ptr->issuffix = (Affix->type == FF_SUFFIX) ? true : false;
				ptr++;
			}
		}
	}
	ptr->affix = NULL;
	Conf->CompoundAffix = (CMPDAffix *) repalloc(Conf->CompoundAffix, sizeof(CMPDAffix) * (ptr - Conf->CompoundAffix + 1));

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
		 * if prefix is a all non-chaged part's length then all word contains
		 * only prefix and suffix, so out
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
		int			err;
		pg_wchar   *data;
		size_t		data_len;
		int			newword_len;

		/* Convert data string to wide characters */
		newword_len = strlen(newword);
		data = (pg_wchar *) palloc((newword_len + 1) * sizeof(pg_wchar));
		data_len = pg_mb2wchar_with_len(newword, data, newword_len);

		if (!(err = pg_regexec(&(Affix->reg.regex), data, data_len, 0, NULL, 0, NULL, 0)))
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
NormalizeSubWord(IspellDict *Conf, char *word, int flag)
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
	if (FindWord(Conf, word, 0, flag))
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
							int			ff = (prefix->aff[j]->flagflags & suffix->aff[i]->flagflags & FF_CROSSPRODUCT) ?
							0 : prefix->aff[j]->flag;

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
		return (NULL);
	}
	return (forms);
}

typedef struct SplitVar
{
	int			nstem;
	int			lenstem;
	char	  **stem;
	struct SplitVar *next;
} SplitVar;

static int
CheckCompoundAffixes(CMPDAffix **ptr, char *word, int len, bool CheckInPlace)
{
	bool		issuffix;

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
SplitToVariants(IspellDict *Conf, SPNode *snode, SplitVar *orig, char *word, int wordlen, int startpos, int minpos)
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
			if (level == FF_COMPOUNDBEGIN)
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
NINormalizeWord(IspellDict *Conf, char *word)
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
