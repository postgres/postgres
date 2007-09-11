#include "postgres.h"

#include <ctype.h>

#include "spell.h"
#include "common.h"
#include "ts_locale.h"

#define MAX_NORM 1024
#define MAXNORMLEN 256

#define ERRSTRSIZE	1024

#define STRNCMP(s,p)	strncmp( (s), (p), strlen(p) )
#define GETWCHAR(W,L,N,T) ( ((uint8*)(W))[ ((T)==FF_PREFIX) ? (N) : ( (L) - 1 - (N) ) ] )
#define GETCHAR(A,N,T)	  GETWCHAR( (A)->repl, (A)->replen, N, T )

static char *VoidString = "";

#define MEMOUT(X)  if ( !(X) ) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")))

static int
cmpspell(const void *s1, const void *s2)
{
	return (strcmp((*(const SPELL **) s1)->word, (*(const SPELL **) s2)->word));
}
static int
cmpspellaffix(const void *s1, const void *s2)
{
	return (strcmp((*(const SPELL **) s1)->p.flag, (*(const SPELL **) s2)->p.flag));
}

static char *
strnduplicate(char *s, int len)
{
	char	   *d = (char *) palloc(len + 1);

	memcpy(d, s, len);
	d[len] = '\0';
	return d;
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

int
NIAddSpell(IspellDict * Conf, const char *word, const char *flag)
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
			Conf->Spell = (SPELL **) palloc(Conf->mspell * sizeof(SPELL *));
		}
	}
	Conf->Spell[Conf->nspell] = (SPELL *) palloc(SPELLHDRSZ + strlen(word) + 1);
	strcpy(Conf->Spell[Conf->nspell]->word, word);
	strncpy(Conf->Spell[Conf->nspell]->p.flag, flag, 16);
	Conf->nspell++;
	return (0);
}


int
NIImportDictionary(IspellDict * Conf, const char *filename)
{
	char		str[BUFSIZ], *pstr;
	FILE	   *dict;

	if (!(dict = fopen(filename, "r")))
		return (1);
	while (fgets(str, sizeof(str), dict))
	{
		char	   *s;
		const char *flag;

		pg_verifymbstr(str, strlen(str), false);

		flag = NULL;
		if ((s = findchar(str, '/')))
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


		s = str;
		while (*s)
		{
			if (t_isspace(s))
			{
				*s = '\0';
				break;
			}
			s += pg_mblen(s);
		}
		pstr = lowerstr(str);

		NIAddSpell(Conf, pstr, flag);
		pfree(pstr);
	}
	fclose(dict);
	return (0);
}


static int
FindWord(IspellDict * Conf, const char *word, int affixflag, char compoundonly)
{
	SPNode	   *node = Conf->Dictionary;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	uint8	   *ptr = (uint8 *) word;

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
					if (compoundonly && !StopMiddle->compoundallow)
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

int
NIAddAffix(IspellDict * Conf, int flag, char flagflags, const char *mask, const char *find, const char *repl, int type)
{
	if (Conf->naffixes >= Conf->maffixes)
	{
		if (Conf->maffixes)
		{
			Conf->maffixes += 16;
			Conf->Affix = (AFFIX *) realloc((void *) Conf->Affix, Conf->maffixes * sizeof(AFFIX));
		}
		else
		{
			Conf->maffixes = 16;
			Conf->Affix = (AFFIX *) malloc(Conf->maffixes * sizeof(AFFIX));
		}
		MEMOUT(Conf->Affix);
	}

	if (strcmp(mask, ".") == 0)
	{
		Conf->Affix[Conf->naffixes].issimple = 1;
		Conf->Affix[Conf->naffixes].isregis = 0;
		Conf->Affix[Conf->naffixes].mask = VoidString;
	}
	else if (RS_isRegis(mask))
	{
		Conf->Affix[Conf->naffixes].issimple = 0;
		Conf->Affix[Conf->naffixes].isregis = 1;
		Conf->Affix[Conf->naffixes].mask = (mask && *mask) ? strdup(mask) : VoidString;
	}
	else
	{
		int			masklen = strlen(mask);

		Conf->Affix[Conf->naffixes].issimple = 0;
		Conf->Affix[Conf->naffixes].isregis = 0;
		Conf->Affix[Conf->naffixes].mask = (char *) malloc(masklen + 2);
		if (type == FF_SUFFIX)
			sprintf(Conf->Affix[Conf->naffixes].mask, "%s$", mask);
		else
			sprintf(Conf->Affix[Conf->naffixes].mask, "^%s", mask);
	}
	MEMOUT(Conf->Affix[Conf->naffixes].mask);

	Conf->Affix[Conf->naffixes].compile = 1;
	Conf->Affix[Conf->naffixes].flagflags = flagflags;
	Conf->Affix[Conf->naffixes].flag = flag;
	Conf->Affix[Conf->naffixes].type = type;

	Conf->Affix[Conf->naffixes].find = (find && *find) ? strdup(find) : VoidString;
	MEMOUT(Conf->Affix[Conf->naffixes].find);
	if ((Conf->Affix[Conf->naffixes].replen = strlen(repl)) > 0)
	{
		Conf->Affix[Conf->naffixes].repl = strdup(repl);
		MEMOUT(Conf->Affix[Conf->naffixes].repl);
	}
	else
		Conf->Affix[Conf->naffixes].repl = VoidString;
	Conf->naffixes++;
	return (0);
}

#define PAE_WAIT_MASK	0
#define PAE_INMASK	1
#define PAE_WAIT_FIND	2
#define PAE_INFIND	3
#define PAE_WAIT_REPL	4
#define PAE_INREPL	5

static bool
parse_affentry(char *str, char *mask, char *find, char *repl, int line)
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
				ts_error(ERROR, "Affix parse error at %d line", line);
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
				ts_error(ERROR, "Affix parse error at %d line", line);
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
				ts_error(ERROR, "Affix parse error at %d line", line);
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
				ts_error(ERROR, "Affix parse error at %d line", line);
		}
		else
			ts_error(ERROR, "Unknown state in parse_affentry: %d", state);

		str += pg_mblen(str);
	}

	*pmask = *pfind = *prepl = '\0';

	return (*mask && (*find || *repl)) ? true : false;
}

int
NIImportAffixes(IspellDict * Conf, const char *filename)
{
	char		str[BUFSIZ], *pstr = NULL;
	char		mask[BUFSIZ];
	char		find[BUFSIZ];
	char		repl[BUFSIZ];
	char	   *s;
	int			suffixes = 0;
	int			prefixes = 0;
	int			flag = 0;
	char		flagflags = 0;
	FILE	   *affix;
	int			line = 0;
	int			oldformat = 0;

	if (!(affix = fopen(filename, "r")))
		return (1);
	Conf->compoundcontrol = '\t';

	while (fgets(str, sizeof(str), affix))
	{
		line++;
		if ( *str == '#' || *str == '\n' )
			continue;

		pg_verifymbstr(str, strlen(str), false);
		if ( pstr )
			pfree( pstr );
		pstr = lowerstr(str);
		if (STRNCMP(pstr, "compoundwords") == 0)
		{
			s = findchar(str, 'l');
			if (s)
			{
				while (*s && !t_isspace(s))
					s++;
				while (*s && t_isspace(s))
					s++;
				if (*s && pg_mblen(s) == 1)
					Conf->compoundcontrol = *s;
				oldformat++;
				continue;
			}
		}
		if (STRNCMP(pstr, "suffixes") == 0)
		{
			suffixes = 1;
			prefixes = 0;
			oldformat++;
			continue;
		}
		if (STRNCMP(pstr, "prefixes") == 0)
		{
			suffixes = 0;
			prefixes = 1;
			oldformat++;
			continue;
		}
		if (STRNCMP(pstr, "flag") == 0)
		{
			s = str + 4;
			flagflags = 0;

			while (*s && t_isspace(s))
				s++;
			oldformat++;

			/* allow only single-encoded flags */
			if (pg_mblen(s) != 1)
				elog(ERROR, "Multiencoded flag at line %d: %s", line, s);

			if (*s == '*')
			{
				flagflags |= FF_CROSSPRODUCT;
				s++;
			}
			else if (*s == '~')
			{
				flagflags |= FF_COMPOUNDONLYAFX;
				s++;
			}

			if (*s == '\\')
				s++;

			/* allow only single-encoded flags */
			if (pg_mblen(s) != 1)
			{
				flagflags = 0;
				elog(ERROR, "Multiencoded flag at line %d: %s", line, s);
			}

			flag = (unsigned char) *s;
			continue;
		}
		if (STRNCMP(str, "COMPOUNDFLAG") == 0 || STRNCMP(str, "COMPOUNDMIN") == 0 ||
			STRNCMP(str, "PFX") == 0 || STRNCMP(str, "SFX") == 0)
		{

			if (oldformat)
				elog(ERROR, "Wrong affix file format");

			fclose(affix);
			return NIImportOOAffixes(Conf, filename);

		}
		if ((!suffixes) && (!prefixes))
			continue;

		if (!parse_affentry(pstr, mask, find, repl, line)) 
			continue;

		NIAddAffix(Conf, flag, flagflags, mask, find, repl, suffixes ? FF_SUFFIX : FF_PREFIX);
	}
	fclose(affix);

	if ( pstr )
		pfree( pstr );

	return (0);
}

int
NIImportOOAffixes(IspellDict * Conf, const char *filename)
{
	char		str[BUFSIZ];
	char		type[BUFSIZ], *ptype = NULL;
	char		sflag[BUFSIZ];
	char		mask[BUFSIZ], *pmask;
	char		find[BUFSIZ], *pfind;
	char		repl[BUFSIZ], *prepl;
	bool		isSuffix = false;
	int			flag = 0;
	char		flagflags = 0;
	FILE	   *affix;
	int			line = 0;
	int			scanread = 0;
	char		scanbuf[BUFSIZ];

	sprintf(scanbuf, "%%6s %%%ds %%%ds %%%ds %%%ds", BUFSIZ / 5, BUFSIZ / 5, BUFSIZ / 5, BUFSIZ / 5);

	if (!(affix = fopen(filename, "r")))
		return (1);
	Conf->compoundcontrol = '\t';

	while (fgets(str, sizeof(str), affix))
	{
		line++;
		if (*str == '\0' || t_isspace(str) || t_iseq(str, '#'))
			continue;
		pg_verifymbstr(str, strlen(str), false);

		if (STRNCMP(str, "COMPOUNDFLAG") == 0)
		{
			char	   *s = str + strlen("COMPOUNDFLAG");

			while (*s && t_isspace(s))
				s++;
			if (*s && pg_mblen(s) == 1)
				Conf->compoundcontrol = *s;
			continue;
		}

		scanread = sscanf(str, scanbuf, type, sflag, find, repl, mask);

		if (ptype)
			pfree(ptype);
		ptype = lowerstr(type);
		if (scanread < 4 || (STRNCMP(ptype, "sfx") && STRNCMP(ptype, "pfx")))
			continue;

		if (scanread == 4)
		{
			if (strlen(sflag) != 1)
				continue;
			flag = *sflag;
			isSuffix = (STRNCMP(ptype, "sfx") == 0) ? true : false;
			pfind = lowerstr(find);
			if (t_iseq(find, 'y'))
				flagflags |= FF_CROSSPRODUCT;
			else
				flagflags = 0;
			pfree(pfind);
		}
		else
		{
			if (strlen(sflag) != 1 || flag != *sflag || flag == 0)
				continue;
			prepl = lowerstr(repl);
			pfind = lowerstr(find);
			pmask = lowerstr(mask);
			if (t_iseq(find, '0'))
				*pfind = '\0';
			if (t_iseq(repl, '0'))
				*prepl = '\0';

			NIAddAffix(Conf, flag, flagflags, pmask, pfind, prepl, isSuffix ? FF_SUFFIX : FF_PREFIX);
			pfree(prepl);
			pfree(pfind);
			pfree(pmask);
		}
	}

	if (ptype)
		pfree(ptype);
	fclose(affix);

	return 0;
}

static int
MergeAffix(IspellDict * Conf, int a1, int a2)
{
	int			naffix = 0;
	char	  **ptr = Conf->AffixData;

	while (*ptr)
	{
		naffix++;
		ptr++;
	}

	Conf->AffixData = (char **) realloc(Conf->AffixData, (naffix + 2) * sizeof(char *));
	MEMOUT(Conf->AffixData);
	ptr = Conf->AffixData + naffix;
	*ptr = malloc(strlen(Conf->AffixData[a1]) + strlen(Conf->AffixData[a2]) + 1 /* space */ + 1 /* \0 */ );
	MEMOUT(ptr);
	sprintf(*ptr, "%s %s", Conf->AffixData[a1], Conf->AffixData[a2]);
	ptr++;
	*ptr = '\0';
	return naffix;
}


static SPNode *
mkSPNode(IspellDict * Conf, int low, int high, int level)
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

	rs = (SPNode *) malloc(SPNHDRSZ + nchar * sizeof(SPNodeData));
	MEMOUT(rs);
	memset(rs, 0, SPNHDRSZ + nchar * sizeof(SPNodeData));
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
				if (data->isword && data->affix != Conf->Spell[i]->p.d.affix)
				{
					/*
					 * fprintf(stderr,"Word already exists: %s (affixes: '%s'
					 * and '%s')\n", Conf->Spell[i]->word,
					 * Conf->AffixData[data->affix],
					 * Conf->AffixData[Conf->Spell[i]->p.d.affix] );
					 */
					/* MergeAffix called a few times */
					data->affix = MergeAffix(Conf, data->affix, Conf->Spell[i]->p.d.affix);
				}
				else
					data->affix = Conf->Spell[i]->p.d.affix;
				data->isword = 1;
				if (strchr(Conf->AffixData[data->affix], Conf->compoundcontrol))
					data->compoundallow = 1;
			}
		}

	data->node = mkSPNode(Conf, lownew, high, level + 1);

	return rs;
}

void
NISortDictionary(IspellDict * Conf)
{
	size_t		i;
	int			naffix = 3;

	/* compress affixes */
	qsort((void *) Conf->Spell, Conf->nspell, sizeof(SPELL *), cmpspellaffix);
	for (i = 1; i < Conf->nspell; i++)
		if (strcmp(Conf->Spell[i]->p.flag, Conf->Spell[i - 1]->p.flag))
			naffix++;

	Conf->AffixData = (char **) malloc(naffix * sizeof(char *));
	MEMOUT(Conf->AffixData);
	memset(Conf->AffixData, 0, naffix * sizeof(char *));
	naffix = 1;
	Conf->AffixData[0] = strdup("");
	MEMOUT(Conf->AffixData[0]);
	Conf->AffixData[1] = strdup(Conf->Spell[0]->p.flag);
	MEMOUT(Conf->AffixData[1]);
	Conf->Spell[0]->p.d.affix = 1;
	Conf->Spell[0]->p.d.len = strlen(Conf->Spell[0]->word);
	for (i = 1; i < Conf->nspell; i++)
	{
		if (strcmp(Conf->Spell[i]->p.flag, Conf->AffixData[naffix]))
		{
			naffix++;
			Conf->AffixData[naffix] = strdup(Conf->Spell[i]->p.flag);
			MEMOUT(Conf->AffixData[naffix]);
		}
		Conf->Spell[i]->p.d.affix = naffix;
		Conf->Spell[i]->p.d.len = strlen(Conf->Spell[i]->word);
	}

	qsort((void *) Conf->Spell, Conf->nspell, sizeof(SPELL *), cmpspell);
	Conf->Dictionary = mkSPNode(Conf, 0, Conf->nspell, 0);

	for (i = 0; i < Conf->nspell; i++)
		pfree(Conf->Spell[i]);
	pfree(Conf->Spell);
	Conf->Spell = NULL;
}

static AffixNode *
mkANode(IspellDict * Conf, int low, int high, int level, int type)
{
	int			i;
	int			nchar = 0;
	uint8		lastchar = '\0';
	AffixNode  *rs;
	AffixNodeData *data;
	int			lownew = low;

	for (i = low; i < high; i++)
		if (Conf->Affix[i].replen > level && lastchar != GETCHAR(Conf->Affix + i, level, type))
		{
			nchar++;
			lastchar = GETCHAR(Conf->Affix + i, level, type);
		}

	if (!nchar)
		return NULL;

	rs = (AffixNode *) malloc(ANHRDSZ + nchar * sizeof(AffixNodeData));
	MEMOUT(rs);
	memset(rs, 0, ANHRDSZ + nchar * sizeof(AffixNodeData));
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
					lownew = i;
					data++;
				}
				lastchar = GETCHAR(Conf->Affix + i, level, type);
			}
			data->val = GETCHAR(Conf->Affix + i, level, type);
			if (Conf->Affix[i].replen == level + 1)
			{					/* affix stopped */
				if (!data->naff)
				{
					data->aff = (AFFIX **) malloc(sizeof(AFFIX *) * (high - i + 1));
					MEMOUT(data->aff);
				}
				data->aff[data->naff] = Conf->Affix + i;
				data->naff++;
			}
		}

	data->node = mkANode(Conf, lownew, high, level + 1, type);

	return rs;
}

static void
mkVoidAffix(IspellDict * Conf, int issuffix, int startsuffix)
{
	int			i,
				cnt = 0;
	int			start = (issuffix) ? startsuffix : 0;
	int			end = (issuffix) ? Conf->naffixes : startsuffix;
	AffixNode  *Affix = (AffixNode *) malloc(ANHRDSZ + sizeof(AffixNodeData));

	MEMOUT(Affix);
	memset(Affix, 0, ANHRDSZ + sizeof(AffixNodeData));
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

	Affix->data->aff = (AFFIX **) malloc(sizeof(AFFIX *) * cnt);
	MEMOUT(Affix->data->aff);
	Affix->data->naff = (uint32) cnt;

	cnt = 0;
	for (i = start; i < end; i++)
		if (Conf->Affix[i].replen == 0)
		{
			Affix->data->aff[cnt] = Conf->Affix + i;
			cnt++;
		}
}

void
NISortAffixes(IspellDict * Conf)
{
	AFFIX	   *Affix;
	size_t		i;
	CMPDAffix  *ptr;
	int			firstsuffix = -1;

	if (Conf->naffixes == 0)
		return;

	if (Conf->naffixes > 1)
		qsort((void *) Conf->Affix, Conf->naffixes, sizeof(AFFIX), cmpaffix);
	Conf->CompoundAffix = ptr = (CMPDAffix *) malloc(sizeof(CMPDAffix) * Conf->naffixes);
	MEMOUT(Conf->CompoundAffix);
	ptr->affix = NULL;

	for (i = 0; i < Conf->naffixes; i++)
	{
		Affix = &(((AFFIX *) Conf->Affix)[i]);
		if (Affix->type == FF_SUFFIX)
		{
			if (firstsuffix < 0)
				firstsuffix = i;
			if ((Affix->flagflags & FF_COMPOUNDONLYAFX) && Affix->replen > 0)
			{
				if (ptr == Conf->CompoundAffix ||
					strbncmp((const unsigned char *) (ptr - 1)->affix,
							 (const unsigned char *) Affix->repl,
							 (ptr - 1)->len))
				{
					/* leave only unique and minimals suffixes */
					ptr->affix = Affix->repl;
					ptr->len = Affix->replen;
					ptr++;
				}
			}
		}
	}
	ptr->affix = NULL;
	Conf->CompoundAffix = (CMPDAffix *) realloc(Conf->CompoundAffix, sizeof(CMPDAffix) * (ptr - Conf->CompoundAffix + 1));

	Conf->Prefix = mkANode(Conf, 0, firstsuffix, 0, FF_PREFIX);
	Conf->Suffix = mkANode(Conf, firstsuffix, Conf->naffixes, 0, FF_SUFFIX);
	mkVoidAffix(Conf, 1, firstsuffix);
	mkVoidAffix(Conf, 0, firstsuffix);
}

static AffixNodeData *
FinfAffixes(AffixNode * node, const char *word, int wrdlen, int *level, int type)
{
	AffixNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	uint8		symbol;

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
CheckAffix(const char *word, size_t len, AFFIX * Affix, char flagflags, char *newword, int *baselen)
{

	if (flagflags & FF_COMPOUNDONLYAFX)
	{
		if ((Affix->flagflags & FF_COMPOUNDONLYAFX) == 0)
			return NULL;
	}
	else
	{
		if (Affix->flagflags & FF_COMPOUNDONLYAFX)
			return NULL;
	}

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

	if (Affix->issimple)
		return newword;
	else if (Affix->isregis)
	{
		if (Affix->compile)
		{
			RS_compile(&(Affix->reg.regis), (Affix->type == FF_SUFFIX) ? true : false, Affix->mask);
			Affix->compile = 0;
		}
		if (RS_execute(&(Affix->reg.regis), newword))
			return newword;
	}
	else
	{
		int			err;
		pg_wchar   *data;
		size_t		data_len;
		int			newword_len;

		if (Affix->compile)
		{
			int			wmasklen,
						masklen = strlen(Affix->mask);
			pg_wchar   *mask;

			mask = (pg_wchar *) palloc((masklen + 1) * sizeof(pg_wchar));
			wmasklen = pg_mb2wchar_with_len(Affix->mask, mask, masklen);

			err = pg_regcomp(&(Affix->reg.regex), mask, wmasklen, REG_ADVANCED | REG_NOSUB);
			pfree(mask);
			if (err)
			{
				char		regerrstr[ERRSTRSIZE];

				pg_regerror(err, &(Affix->reg.regex), regerrstr, ERRSTRSIZE);
				elog(ERROR, "regex error in '%s': %s", Affix->mask, regerrstr);
			}
			Affix->compile = 0;
		}

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


static char **
NormalizeSubWord(IspellDict * Conf, char *word, char flag)
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
	if (FindWord(Conf, word, 0, flag & FF_COMPOUNDWORD))
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
		prefix = FinfAffixes(pnode, word, wrdlen, &plevel, FF_PREFIX);
		if (!prefix)
			break;
		for (j = 0; j < prefix->naff; j++)
		{
			if (CheckAffix(word, wrdlen, prefix->aff[j], flag, newword, NULL))
			{
				/* prefix success */
				if (FindWord(Conf, newword, prefix->aff[j]->flag, flag & FF_COMPOUNDWORD) && (cur - forms) < (MAX_NORM - 1))
				{
					/* word search success */
					*cur = pstrdup(newword);
					cur++;
					*cur = NULL;
				}
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
		suffix = FinfAffixes(snode, word, wrdlen, &slevel, FF_SUFFIX);
		if (!suffix)
			break;
		/* foreach suffix check affix */
		for (i = 0; i < suffix->naff; i++)
		{
			if (CheckAffix(word, wrdlen, suffix->aff[i], flag, newword, &baselen))
			{
				/* suffix success */
				if (FindWord(Conf, newword, suffix->aff[i]->flag, flag & FF_COMPOUNDWORD) && (cur - forms) < (MAX_NORM - 1))
				{
					/* word search success */
					*cur = pstrdup(newword);
					cur++;
					*cur = NULL;
				}
				/* now we will look changed word with prefixes */
				pnode = Conf->Prefix;
				plevel = 0;
				swrdlen = strlen(newword);
				while (pnode)
				{
					prefix = FinfAffixes(pnode, newword, swrdlen, &plevel, FF_PREFIX);
					if (!prefix)
						break;
					for (j = 0; j < prefix->naff; j++)
					{
						if (CheckAffix(newword, swrdlen, prefix->aff[j], flag, pnewword, &baselen))
						{
							/* prefix success */
							int			ff = (prefix->aff[j]->flagflags & suffix->aff[i]->flagflags & FF_CROSSPRODUCT) ?
							0 : prefix->aff[j]->flag;

							if (FindWord(Conf, pnewword, ff, flag & FF_COMPOUNDWORD) && (cur - forms) < (MAX_NORM - 1))
							{
								/* word search success */
								*cur = pstrdup(pnewword);
								cur++;
								*cur = NULL;
							}
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
	char	  **stem;
	struct SplitVar *next;
}	SplitVar;

static int
CheckCompoundAffixes(CMPDAffix ** ptr, char *word, int len, bool CheckInPlace)
{
	if (CheckInPlace)
	{
		while ((*ptr)->affix)
		{
			if (len > (*ptr)->len && strncmp((*ptr)->affix, word, (*ptr)->len) == 0)
			{
				len = (*ptr)->len;
				(*ptr)++;
				return len;
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
				(*ptr)++;
				return len;
			}
			(*ptr)++;
		}
	}
	return 0;
}

static SplitVar *
CopyVar(SplitVar * s, int makedup)
{
	SplitVar   *v = (SplitVar *) palloc(sizeof(SplitVar));

	v->stem = (char **) palloc(sizeof(char *) * (MAX_NORM));
	v->next = NULL;
	if (s)
	{
		int			i;

		v->nstem = s->nstem;
		for (i = 0; i < s->nstem; i++)
			v->stem[i] = (makedup) ? pstrdup(s->stem[i]) : s->stem[i];
	}
	else
		v->nstem = 0;
	return v;
}


static SplitVar *
SplitToVariants(IspellDict * Conf, SPNode * snode, SplitVar * orig, char *word, int wordlen, int startpos, int minpos)
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

	notprobed = (char *) palloc(wordlen);
	memset(notprobed, 1, wordlen);
	var = CopyVar(orig, 1);

	while (level < wordlen)
	{
		/* find word with epenthetic or/and compound suffix */
		caff = Conf->CompoundAffix;
		while (level > startpos && (lenaff = CheckCompoundAffixes(&caff, word + level, wordlen - level, (node) ? true : false)) > 0)
		{
			/*
			 * there is one of compound suffixes, so check word for existings
			 */
			char		buf[MAXNORMLEN];
			char	  **subres;

			lenaff = level - startpos + lenaff;

			if (!notprobed[startpos + lenaff - 1])
				continue;

			if (level + lenaff - 1 <= minpos)
				continue;

			memcpy(buf, word + startpos, lenaff);
			buf[lenaff] = '\0';

			subres = NormalizeSubWord(Conf, buf, FF_COMPOUNDWORD | FF_COMPOUNDONLYAFX);
			if (subres)
			{
				/* Yes, it was a word from dictionary */
				SplitVar   *new = CopyVar(var, 0);
				SplitVar   *ptr = var;
				char	  **sptr = subres;

				notprobed[startpos + lenaff - 1] = 0;

				while (*sptr)
				{
					new->stem[new->nstem] = *sptr;
					new->nstem++;
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

			/* find infinitive */
			if (StopMiddle->isword && StopMiddle->compoundallow && notprobed[level])
			{
				/* ok, we found full compoundallowed word */
				if (level > minpos)
				{
					/* and its length more than minimal */
					if (wordlen == level + 1)
					{
						/* well, it was last word */
						var->stem[var->nstem] = strnduplicate(word + startpos, wordlen - startpos);
						var->nstem++;
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
						var->stem[var->nstem] = strnduplicate(word + startpos, level - startpos);
						var->nstem++;
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

	var->stem[var->nstem] = strnduplicate(word + startpos, wordlen - startpos);
	var->nstem++;
	pfree(notprobed);
	return var;
}

TSLexeme *
NINormalizeWord(IspellDict * Conf, char *uword)
{
	char	  **res;
	char	   *word;
	TSLexeme   *lcur = NULL,
			   *lres = NULL;
	uint16		NVariant = 1;

	word = lowerstr(uword);
	res = NormalizeSubWord(Conf, word, 0);

	if (res)
	{
		char	  **ptr = res;

		lcur = lres = (TSLexeme *) palloc(MAX_NORM * sizeof(TSLexeme));
		while (*ptr)
		{
			lcur->lexeme = *ptr;
			lcur->flags = 0;
			lcur->nvariant = NVariant++;
			lcur++;
			ptr++;
		}
		lcur->lexeme = NULL;
		pfree(res);
	}

	if (Conf->compoundcontrol != '\t')
	{
		int			wordlen = strlen(word);
		SplitVar   *ptr,
				   *var = SplitToVariants(Conf, NULL, NULL, word, wordlen, 0, -1);
		int			i;

		while (var)
		{
			if (var->nstem > 1)
			{
				char	  **subres = NormalizeSubWord(Conf, var->stem[var->nstem - 1], FF_COMPOUNDWORD);

				if (subres)
				{
					char	  **subptr = subres;

					if (!lcur)
						lcur = lres = (TSLexeme *) palloc(MAX_NORM * sizeof(TSLexeme));

					while (*subptr)
					{
						for (i = 0; i < var->nstem - 1; i++)
						{
							lcur->lexeme = (subptr == subres) ? var->stem[i] : pstrdup(var->stem[i]);
							lcur->flags = 0;
							lcur->nvariant = NVariant;
							lcur++;
						}

						lcur->lexeme = *subptr;
						lcur->flags = 0;
						lcur->nvariant = NVariant;
						lcur++;
						subptr++;
						NVariant++;
					}

					lcur->lexeme = NULL;
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

	pfree(word);

	return lres;
}


static void
freeSPNode(SPNode * node)
{
	SPNodeData *data;

	if (!node)
		return;
	data = node->data;
	while (node->length)
	{
		freeSPNode(data->node);
		data++;
		node->length--;
	}
	free(node);
}

static void
freeANode(AffixNode * node)
{
	AffixNodeData *data;

	if (!node)
		return;
	data = node->data;
	while (node->length)
	{
		freeANode(data->node);
		if (data->naff)
			free(data->aff);
		data++;
		node->length--;
	}
	free(node);
}


void
NIFree(IspellDict * Conf)
{
	int			i;
	AFFIX	   *Affix = (AFFIX *) Conf->Affix;
	char	  **aff = Conf->AffixData;

	if (aff)
	{
		while (*aff)
		{
			free(*aff);
			aff++;
		}
		free(Conf->AffixData);
	}


	for (i = 0; i < Conf->naffixes; i++)
	{
		if (Affix[i].compile == 0)
		{
			if (Affix[i].isregis)
				RS_free(&(Affix[i].reg.regis));
			else
				pg_regfree(&(Affix[i].reg.regex));
		}
		if (Affix[i].mask != VoidString)
			free(Affix[i].mask);
		if (Affix[i].find != VoidString)
			free(Affix[i].find);
		if (Affix[i].repl != VoidString)
			free(Affix[i].repl);
	}
	if (Conf->Spell)
	{
		for (i = 0; i < Conf->nspell; i++)
			pfree(Conf->Spell[i]);
		pfree(Conf->Spell);
	}

	if (Conf->Affix)
		free(Conf->Affix);
	if (Conf->CompoundAffix)
		free(Conf->CompoundAffix);
	freeSPNode(Conf->Dictionary);
	freeANode(Conf->Suffix);
	freeANode(Conf->Prefix);
	memset((void *) Conf, 0, sizeof(IspellDict));
	return;
}
