#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"

#include "spell.h"

#define MAXNORMLEN 56

#define STRNCASECMP(x,y)		(strncasecmp(x,y,strlen(y)))

static int
cmpspell(const void *s1, const void *s2)
{
	return (strcmp(((const SPELL *) s1)->word, ((const SPELL *) s2)->word));
}

static void
strlower(char *str)
{
	unsigned char *ptr = (unsigned char *) str;

	while (*ptr)
	{
		*ptr = tolower(*ptr);
		ptr++;
	}
}

/* backward string compaire for suffix tree operations */
static int
strbcmp(const char *s1, const char *s2)
{
	int			l1 = strlen(s1) - 1,
				l2 = strlen(s2) - 1;

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
strbncmp(const char *s1, const char *s2, size_t count)
{
	int			l1 = strlen(s1) - 1,
				l2 = strlen(s2) - 1,
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
	if (((const AFFIX *) s1)->type < ((const AFFIX *) s2)->type)
		return -1;
	if (((const AFFIX *) s1)->type > ((const AFFIX *) s2)->type)
		return 1;
	if (((const AFFIX *) s1)->type == 'p')
		return (strcmp(((const AFFIX *) s1)->repl, ((const AFFIX *) s2)->repl));
	else
		return (strbcmp(((const AFFIX *) s1)->repl, ((const AFFIX *) s2)->repl));
}

int
AddSpell(IspellDict * Conf, const char *word, const char *flag)
{
	if (Conf->nspell >= Conf->mspell)
	{
		if (Conf->mspell)
		{
			Conf->mspell += 1024 * 20;
			Conf->Spell = (SPELL *) realloc(Conf->Spell, Conf->mspell * sizeof(SPELL));
		}
		else
		{
			Conf->mspell = 1024 * 20;
			Conf->Spell = (SPELL *) malloc(Conf->mspell * sizeof(SPELL));
		}
		if (Conf->Spell == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}
	Conf->Spell[Conf->nspell].word = strdup(word);
	if (!Conf->Spell[Conf->nspell].word)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	strncpy(Conf->Spell[Conf->nspell].flag, flag, 10);
	Conf->nspell++;
	return (0);
}


int
ImportDictionary(IspellDict * Conf, const char *filename)
{
	unsigned char str[BUFSIZ];
	FILE	   *dict;

	if (!(dict = fopen(filename, "r")))
		return (1);
	while (fgets(str, sizeof(str), dict))
	{
		unsigned char *s;
		const unsigned char *flag;

		flag = NULL;
		if ((s = strchr(str, '/')))
		{
			*s = 0;
			s++;
			flag = s;
			while (*s)
			{
				if (((*s >= 'A') && (*s <= 'Z')) || ((*s >= 'a') && (*s <= 'z')))
					s++;
				else
				{
					*s = 0;
					break;
				}
			}
		}
		else
			flag = "";
		strlower(str);
		/* Dont load words if first letter is not required */
		/* It allows to optimize loading at  search time   */
		s = str;
		while (*s)
		{
			if (*s == '\r')
				*s = 0;
			if (*s == '\n')
				*s = 0;
			s++;
		}
		AddSpell(Conf, str, flag);
	}
	fclose(dict);
	return (0);
}


static SPELL *
FindWord(IspellDict * Conf, const char *word, int affixflag)
{
	int			l,
				c,
				r,
				resc,
				resl,
				resr,
				i;

	i = (int) (*word) & 255;
	l = Conf->SpellTree.Left[i];
	r = Conf->SpellTree.Right[i];
	if (l == -1)
		return (NULL);
	while (l <= r)
	{
		c = (l + r) >> 1;
		resc = strcmp(Conf->Spell[c].word, word);
		if ((resc == 0) &&
			((affixflag == 0) || (strchr(Conf->Spell[c].flag, affixflag) != NULL)))
			return (&Conf->Spell[c]);
		resl = strcmp(Conf->Spell[l].word, word);
		if ((resl == 0) &&
			((affixflag == 0) || (strchr(Conf->Spell[l].flag, affixflag) != NULL)))
			return (&Conf->Spell[l]);
		resr = strcmp(Conf->Spell[r].word, word);
		if ((resr == 0) &&
			((affixflag == 0) || (strchr(Conf->Spell[r].flag, affixflag) != NULL)))
			return (&Conf->Spell[r]);
		if (resc < 0)
		{
			l = c + 1;
			r--;
		}
		else if (resc > 0)
		{
			r = c - 1;
			l++;
		}
		else
		{
			l++;
			r--;
		}
	}
	return (NULL);
}

int
AddAffix(IspellDict * Conf, int flag, const char *mask, const char *find, const char *repl, int type)
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
		if (Conf->Affix == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}
	if (type == 's')
		sprintf(Conf->Affix[Conf->naffixes].mask, "%s$", mask);
	else
		sprintf(Conf->Affix[Conf->naffixes].mask, "^%s", mask);
	Conf->Affix[Conf->naffixes].compile = 1;
	Conf->Affix[Conf->naffixes].flag = flag;
	Conf->Affix[Conf->naffixes].type = type;

	strcpy(Conf->Affix[Conf->naffixes].find, find);
	strcpy(Conf->Affix[Conf->naffixes].repl, repl);
	Conf->Affix[Conf->naffixes].replen = strlen(repl);
	Conf->naffixes++;
	return (0);
}

static char *
remove_spaces(char *dist, char *src)
{
	char	   *d,
			   *s;

	d = dist;
	s = src;
	while (*s)
	{
		if (*s != ' ' && *s != '-' && *s != '\t')
		{
			*d = *s;
			d++;
		}
		s++;
	}
	*d = 0;
	return (dist);
}


int
ImportAffixes(IspellDict * Conf, const char *filename)
{
	unsigned char str[BUFSIZ];
	unsigned char flag = 0;
	unsigned char mask[BUFSIZ] = "";
	unsigned char find[BUFSIZ] = "";
	unsigned char repl[BUFSIZ] = "";
	unsigned char *s;
	int			i;
	int			suffixes = 0;
	int			prefixes = 0;
	FILE	   *affix;

	if (!(affix = fopen(filename, "r")))
		return (1);

	while (fgets(str, sizeof(str), affix))
	{
		if (!STRNCASECMP(str, "suffixes"))
		{
			suffixes = 1;
			prefixes = 0;
			continue;
		}
		if (!STRNCASECMP(str, "prefixes"))
		{
			suffixes = 0;
			prefixes = 1;
			continue;
		}
		if (!STRNCASECMP(str, "flag "))
		{
			s = str + 5;
			while (strchr("* ", *s))
				s++;
			flag = *s;
			continue;
		}
		if ((!suffixes) && (!prefixes))
			continue;
		if ((s = strchr(str, '#')))
			*s = 0;
		if (!*str)
			continue;
		strlower(str);
		strcpy(mask, "");
		strcpy(find, "");
		strcpy(repl, "");
		i = sscanf(str, "%[^>\n]>%[^,\n],%[^\n]", mask, find, repl);
		remove_spaces(str, repl);
		strcpy(repl, str);
		remove_spaces(str, find);
		strcpy(find, str);
		remove_spaces(str, mask);
		strcpy(mask, str);
		switch (i)
		{
			case 3:
				break;
			case 2:
				if (*find != '\0')
				{
					strcpy(repl, find);
					strcpy(find, "");
				}
				break;
			default:
				continue;
		}

		AddAffix(Conf, (int) flag, mask, find, repl, suffixes ? 's' : 'p');

	}
	fclose(affix);

	return (0);
}

void
SortDictionary(IspellDict * Conf)
{
	int			CurLet = -1,
				Let;
	size_t		i;

	qsort((void *) Conf->Spell, Conf->nspell, sizeof(SPELL), cmpspell);

	for (i = 0; i < 256; i++)
		Conf->SpellTree.Left[i] = -1;

	for (i = 0; i < Conf->nspell; i++)
	{
		Let = (int) (*(Conf->Spell[i].word)) & 255;
		if (CurLet != Let)
		{
			Conf->SpellTree.Left[Let] = i;
			CurLet = Let;
		}
		Conf->SpellTree.Right[Let] = i;
	}
}

void
SortAffixes(IspellDict * Conf)
{
	int			CurLetP = -1,
				CurLetS = -1,
				Let;
	AFFIX	   *Affix;
	size_t		i;

	if (Conf->naffixes > 1)
		qsort((void *) Conf->Affix, Conf->naffixes, sizeof(AFFIX), cmpaffix);
	for (i = 0; i < 256; i++)
	{
		Conf->PrefixTree.Left[i] = Conf->PrefixTree.Right[i] = -1;
		Conf->SuffixTree.Left[i] = Conf->SuffixTree.Right[i] = -1;
	}

	for (i = 0; i < Conf->naffixes; i++)
	{
		Affix = &(((AFFIX *) Conf->Affix)[i]);
		if (Affix->type == 'p')
		{
			Let = (int) (*(Affix->repl)) & 255;
			if (CurLetP != Let)
			{
				Conf->PrefixTree.Left[Let] = i;
				CurLetP = Let;
			}
			Conf->PrefixTree.Right[Let] = i;
		}
		else
		{
			Let = (Affix->replen) ? (int) (Affix->repl[Affix->replen - 1]) & 255 : 0;
			if (CurLetS != Let)
			{
				Conf->SuffixTree.Left[Let] = i;
				CurLetS = Let;
			}
			Conf->SuffixTree.Right[Let] = i;
		}
	}
}

static char *
CheckSuffix(const char *word, size_t len, AFFIX * Affix, int *res, IspellDict * Conf)
{
	regmatch_t	subs[2];		/* workaround for apache&linux */
	char		newword[2 * MAXNORMLEN] = "";
	int			err;

	*res = strbncmp(word, Affix->repl, Affix->replen);
	if (*res < 0)
		return NULL;
	if (*res > 0)
		return NULL;
	strcpy(newword, word);
	strcpy(newword + len - Affix->replen, Affix->find);

	if (Affix->compile)
	{
		err = regcomp(&(Affix->reg), Affix->mask, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (err)
		{
			/* regerror(err, &(Affix->reg), regerrstr, ERRSTRSIZE); */
			regfree(&(Affix->reg));
			return (NULL);
		}
		Affix->compile = 0;
	}
	if (!(err = regexec(&(Affix->reg), newword, 1, subs, 0)))
	{
		if (FindWord(Conf, newword, Affix->flag))
			return pstrdup(newword);
	}
	return NULL;
}

#define NS 1
#define MAX_NORM 512
static int
CheckPrefix(const char *word, size_t len, AFFIX * Affix, IspellDict * Conf, int pi,
			char **forms, char ***cur)
{
	regmatch_t	subs[NS * 2];
	char		newword[2 * MAXNORMLEN] = "";
	int			err,
				ls,
				res,
				lres;
	size_t		newlen;
	AFFIX	   *CAffix = Conf->Affix;

	res = strncmp(word, Affix->repl, Affix->replen);
	if (res != 0)
		return res;
	strcpy(newword, Affix->find);
	strcat(newword, word + Affix->replen);

	if (Affix->compile)
	{
		err = regcomp(&(Affix->reg), Affix->mask, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (err)
		{
			/* regerror(err, &(Affix->reg), regerrstr, ERRSTRSIZE); */
			regfree(&(Affix->reg));
			return (0);
		}
		Affix->compile = 0;
	}
	if (!(err = regexec(&(Affix->reg), newword, 1, subs, 0)))
	{
		SPELL	   *curspell;

		if ((curspell = FindWord(Conf, newword, Affix->flag)))
		{
			if ((*cur - forms) < (MAX_NORM - 1))
			{
				**cur = pstrdup(newword);
				(*cur)++;
				**cur = NULL;
			}
		}
		newlen = strlen(newword);
		ls = Conf->SuffixTree.Left[pi];
		if (ls >= 0 && ((*cur - forms) < (MAX_NORM - 1)))
		{
			**cur = CheckSuffix(newword, newlen, &CAffix[ls], &lres, Conf);
			if (**cur)
			{
				(*cur)++;
				**cur = NULL;
			}
		}
	}
	return 0;
}


char	  **
NormalizeWord(IspellDict * Conf, char *word)
{
/*regmatch_t subs[NS];*/
	size_t		len;
	char	  **forms;
	char	  **cur;
	AFFIX	   *Affix;
	int			ri,
				pi,
				ipi,
				lp,
				rp,
				cp,
				ls,
				rs;
	int			lres,
				rres,
				cres = 0;
	SPELL	   *spell;

	len = strlen(word);
	if (len > MAXNORMLEN)
		return (NULL);

	strlower(word);

	forms = (char **) palloc(MAX_NORM * sizeof(char **));
	cur = forms;
	*cur = NULL;

	ri = (int) (*word) & 255;
	pi = (int) (word[strlen(word) - 1]) & 255;
	Affix = (AFFIX *) Conf->Affix;

	/* Check that the word itself is normal form */
	if ((spell = FindWord(Conf, word, 0)))
	{
		*cur = pstrdup(word);
		cur++;
		*cur = NULL;
	}

	/* Find all other NORMAL forms of the 'word' */

	for (ipi = 0; ipi <= pi; ipi += pi)
	{

		/* check prefix */
		lp = Conf->PrefixTree.Left[ri];
		rp = Conf->PrefixTree.Right[ri];
		while (lp >= 0 && lp <= rp)
		{
			cp = (lp + rp) >> 1;
			cres = 0;
			if ((cur - forms) < (MAX_NORM - 1))
				cres = CheckPrefix(word, len, &Affix[cp], Conf, ipi, forms, &cur);
			if ((lp < cp) && ((cur - forms) < (MAX_NORM - 1)))
				lres = CheckPrefix(word, len, &Affix[lp], Conf, ipi, forms, &cur);
			if ((rp > cp) && ((cur - forms) < (MAX_NORM - 1)))
				rres = CheckPrefix(word, len, &Affix[rp], Conf, ipi, forms, &cur);
			if (cres < 0)
			{
				rp = cp - 1;
				lp++;
			}
			else if (cres > 0)
			{
				lp = cp + 1;
				rp--;
			}
			else
			{
				lp++;
				rp--;
			}
		}

		/* check suffix */
		ls = Conf->SuffixTree.Left[ipi];
		rs = Conf->SuffixTree.Right[ipi];
		while (ls >= 0 && ls <= rs)
		{
			if (((cur - forms) < (MAX_NORM - 1)))
			{
				*cur = CheckSuffix(word, len, &Affix[ls], &lres, Conf);
				if (*cur)
				{
					cur++;
					*cur = NULL;
				}
			}
			if ((rs > ls) && ((cur - forms) < (MAX_NORM - 1)))
			{
				*cur = CheckSuffix(word, len, &Affix[rs], &rres, Conf);
				if (*cur)
				{
					cur++;
					*cur = NULL;
				}
			}
			ls++;
			rs--;
		}						/* end while */

	}							/* for ipi */

	if (cur == forms)
	{
		pfree(forms);
		return (NULL);
	}
	return (forms);
}

void
FreeIspell(IspellDict * Conf)
{
	int			i;
	AFFIX	   *Affix = (AFFIX *) Conf->Affix;

	for (i = 0; i < Conf->naffixes; i++)
	{
		if (Affix[i].compile == 0)
			regfree(&(Affix[i].reg));
	}
	for (i = 0; i < Conf->naffixes; i++)
		free(Conf->Spell[i].word);
	free(Conf->Affix);
	free(Conf->Spell);
	memset((void *) Conf, 0, sizeof(IspellDict));
	return;
}
