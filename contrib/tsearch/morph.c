/*
 * morphology module
 * New dictionary is include in dict.h. For languages which
 * use latin charset it may be need to modify mapdict table.
 * Teodor Sigaev <teodor@stack.net>
 */
#include "postgres.h"

#include <locale.h>

#include "utils/builtins.h"

#include "morph.h"
#include "deflex.h"

/*
 * Struct for calling dictionaries
 * All of this methods are optional, but
 * if all methods are NULL, then dictionary does nothing :)
 * Return value of lemmatize must be palloced or the same.
 * Return value of init must be malloced in other case
 * it will be free in end of transaction!
 */
typedef struct
{
	char		localename[NAMEDATALEN];
	/* init dictionary */
	void	   *(*init) (void);
	/* close dictionary */
	void		(*close) (void *);
	/* find in dictionary */
	char	   *(*lemmatize) (void *, char *, int *);
	int			(*is_stoplemm) (void *, char *, int);
	int			(*is_stemstoplemm) (void *, char *, int);
}	DICT;

/* insert all dictionaries */
#define DICT_BODY
#include "dict.h"
#undef	DICT_BODY

/* fill dictionary's structure */
#define DICT_TABLE
DICT		dicts[] = {
	{
		"C", NULL, NULL, NULL, NULL, NULL		/* fake dictionary */
	}
#include "dict.h"
};

#undef DICT_TABLE

/* array for storing dictionary's objects (if needed) */
void	   *dictobjs[
					 lengthof(dicts)];

#define STOPLEXEM	-2
#define BYLOCALE	-1
#define NODICT		0
#define DEFAULTDICT 1

#define MAXNDICT	2
typedef int2 MAPDICT[MAXNDICT];

#define GETDICT(x,i)	*( ((int2*)(x)) + (i) )

/* map dictionaries for lexem type */
static MAPDICT mapdict[] = {
	{NODICT, NODICT},			/* not used			*/
	{DEFAULTDICT, NODICT},		/* LATWORD		*/
	{BYLOCALE, NODICT},			/* NONLATINWORD		*/
	{BYLOCALE, DEFAULTDICT},	/* UWORD		*/
	{NODICT, NODICT},			/* EMAIL		*/
	{NODICT, NODICT},			/* FURL			*/
	{NODICT, NODICT},			/* HOST			*/
	{NODICT, NODICT},			/* SCIENTIFIC		*/
	{NODICT, NODICT},			/* VERSIONNUMBER		*/
	{BYLOCALE, DEFAULTDICT},	/* PARTHYPHENWORD		*/
	{BYLOCALE, NODICT},			/* CYRPARTHYPHENWORD */
	{DEFAULTDICT, NODICT},		/* LATPARTHYPHENWORD		*/
	{STOPLEXEM, NODICT},		/* SPACE		*/
	{STOPLEXEM, NODICT},		/* TAG		*/
	{STOPLEXEM, NODICT},		/* HTTP			*/
	{BYLOCALE, DEFAULTDICT},	/* HYPHENWORD		*/
	{DEFAULTDICT, NODICT},		/* LATHYPHENWORD		*/
	{BYLOCALE, NODICT},			/* CYRHYPHENWORD	*/
	{NODICT, NODICT},			/* URI			*/
	{NODICT, NODICT},			/* FILEPATH		*/
	{NODICT, NODICT},			/* DECIMAL		*/
	{NODICT, NODICT},			/* SIGNEDINT		*/
	{NODICT, NODICT},			/* UNSIGNEDINT		*/
	{STOPLEXEM, NODICT}			/* HTMLENTITY		*/
};

static bool inited = false;

void
initmorph(void)
{
	int			i,
				j,
				k;
	MAPDICT    *md;
	bool		needinit[lengthof(dicts)];
	const char *curlocale;
	int			bylocaledict = NODICT;

	if (inited)
		return;
	for (i = 1; i < lengthof(dicts); i++)
		needinit[i] = false;

	curlocale = setlocale(LC_CTYPE, NULL);
	if (curlocale)
	{
		for (i = 1; i < lengthof(dicts); i++)
			if (strcmp(dicts[i].localename, curlocale) == 0)
			{
				bylocaledict = i;
				break;
			}
	}

	for (i = 1; i < lengthof(mapdict); i++)
	{
		k = 0;
		md = &mapdict[i];
		for (j = 0; j < MAXNDICT; j++)
		{
			GETDICT(md, k) = GETDICT(md, j);
			if (GETDICT(md, k) == NODICT)
				break;
			else if (GETDICT(md, k) == BYLOCALE)
			{
				if (bylocaledict == NODICT)
					continue;
				GETDICT(md, k) = bylocaledict;
			}
			if (GETDICT(md, k) >= (int2) lengthof(dicts))
				continue;
			needinit[GETDICT(md, k)] = true;
			k++;
		}
		for (; k < MAXNDICT; k++)
			if (GETDICT(md, k) != STOPLEXEM)
				GETDICT(md, k) = NODICT;
	}

	for (i = 1; i < lengthof(dicts); i++)
		if (needinit[i] && dicts[i].init)
			dictobjs[i] = (*(dicts[i].init)) ();

	inited = true;
	return;
}

char *
lemmatize(char *word, int *len, int type)
{
	int2		nd;
	int			i;
	DICT	   *dict;

	for (i = 0; i < MAXNDICT; i++)
	{
		nd = GETDICT(&mapdict[type], i);
		if (nd == NODICT)
		{
			/* there is no dictionary */
			return word;
		}
		else if (nd == STOPLEXEM)
		{
			/* word is stopword */
			return NULL;
		}
		else if (nd == BYLOCALE)
		{
			continue;			/* no dict for current locale */
		}
		else
		{
			dict = &dicts[nd];
			if (dict->is_stoplemm && (*(dict->is_stoplemm)) (dictobjs[nd], word, *len))
				return NULL;
			if (dict->lemmatize)
			{
				int			oldlen = *len;
				char	   *newword = (*(dict->lemmatize)) (dictobjs[nd], word, len);

				/* word is recognized by dictionary */
				if (newword != word || *len != oldlen)
				{
					if (dict->is_stemstoplemm &&
					(*(dict->is_stemstoplemm)) (dictobjs[nd], word, *len))
					{
						if (newword != word && newword)
							pfree(newword);
						return NULL;
					}
					return newword;
				}
			}
		}
	}

	return word;
}

bool
is_stoptype(int type)
{
	return (GETDICT(&mapdict[type], 0) == STOPLEXEM) ? true : false;
}
